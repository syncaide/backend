#include "logging.h"
#include "server/http.h"

server::Http::Http(
    tcp::socket socket,
    flat_buffer buffer,
    tribool secured,
    context &ctx,
    string &root
) : _strand(socket.get_executor().context().get_executor()),
    _timer(
        socket.get_executor().context(),
        chrono::time_point<chrono::steady_clock>::max()
    ),
    _socket(move(socket)),
    _stream(_socket, ctx),
    _buffer(move(buffer)),
    _secured(secured),
    _queue(*this),
    _ctx(ctx),
    _root(root) {}

void server::Http::run() {
    on_timer({});
    if (_secured) {
        _timer.expires_after(chrono::seconds(15));
        _stream.async_handshake(
            ssl::stream_base::server,
            _buffer.data(),
            bind_executor(
                _strand,
                bind(
                    &Http::on_handshake,
                    shared_from_this(),
                    placeholders::_1,
                    placeholders::_2
                )
            )
        );
    } else {
        read();
    }
}

void server::Http::read() {
    _timer.expires_after(chrono::seconds(15));
    if (_secured) {
        http::async_read(
            _stream,
            _buffer,
            _req,
            bind_executor(
                _strand,
                bind(
                    &Http::on_read,
                    shared_from_this(),
                    placeholders::_1
                )
            )
        );
    } else {
        http::async_read(
            _socket,
            _buffer,
            _req,
            bind_executor(
                _strand,
                bind(
                    &Http::on_read,
                    shared_from_this(),
                    placeholders::_1
                )
            )
        );
    }
}

void server::Http::eof() {
    if (_secured) {
        _eof = true;
        _timer.expires_after(chrono::seconds(15));
        _stream.async_shutdown(
            bind_executor(
                _strand,
                bind(
                    &Http::on_shutdown,
                    shared_from_this(),
                    placeholders::_1
                )
            )
        );
    } else {
        error_code code;
        _socket.shutdown(tcp::socket::shutdown_send, code);
    }
}

void server::Http::timeout() {
    if (_secured) {
        if (_eof) {
            return;
        }

        _timer.expires_at(
            chrono::time_point<chrono::steady_clock>::max()
        );
        on_timer({});
        eof();
    } else {
        error_code code;
        _socket.shutdown(tcp::socket::shutdown_both, code);
        _socket.close(code);
    }
}

boost::string_view server::Http::mime_type(string_view path) {
    using boost::beast::iequals;
    auto const ext = [&path] {
        auto const pos = path.rfind(".");
        if (pos == boost::beast::string_view::npos)
            return boost::beast::string_view{};
        return path.substr(pos);
    }();
    if (iequals(ext, ".htm")) return "text/html";
    if (iequals(ext, ".html")) return "text/html";
    if (iequals(ext, ".php")) return "text/html";
    if (iequals(ext, ".css")) return "text/css";
    if (iequals(ext, ".txt")) return "text/plain";
    if (iequals(ext, ".js")) return "application/javascript";
    if (iequals(ext, ".json")) return "application/json";
    if (iequals(ext, ".xml")) return "application/xml";
    if (iequals(ext, ".swf")) return "application/x-shockwave-flash";
    if (iequals(ext, ".flv")) return "video/x-flv";
    if (iequals(ext, ".png")) return "image/png";
    if (iequals(ext, ".jpe")) return "image/jpeg";
    if (iequals(ext, ".jpeg")) return "image/jpeg";
    if (iequals(ext, ".jpg")) return "image/jpeg";
    if (iequals(ext, ".gif")) return "image/gif";
    if (iequals(ext, ".bmp")) return "image/bmp";
    if (iequals(ext, ".ico")) return "image/vnd.microsoft.icon";
    if (iequals(ext, ".tiff")) return "image/tiff";
    if (iequals(ext, ".tif")) return "image/tiff";
    if (iequals(ext, ".svg")) return "image/svg+xml";
    if (iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/text";
}

string server::Http::path_cat(string_view base, string_view path) {
    if (base.empty()) {
        return path.to_string();
    }
    std::string result = base.to_string();
    char constexpr path_separator = '/';
    if (result.back() == path_separator) {
        result.resize(result.size() - 1);
    }
    result.append(path.data(), path.size());
    return result;
}

void server::Http::request_handler(
    string_view root,
    http::request<http::string_body> &&req,
    queue &send
) {
    auto fields = json::object();
    for (auto &field : _req) {
        fields.emplace(string(field.name_string()), string(field.value()));
    }

    tcp::endpoint remote = _socket.remote_endpoint();
    json extra = {
        {"remote",
            {
                {"addr", remote.address().to_string()},
                {"port", remote.port()},
            }
        },
        {"method", string(
            _req.method_string().data(),
            _req.method_string().size()
        )},
        {"target", string(_req.target().data(), _req.target().size())},
        {"version",
            to_string(_req.version() / 10)
            + "."
            + to_string(_req.version() % 10)
        },
        {"fields", fields}
    };
    LOG(info) << logging::add_value("Extra", extra.dump());

    // Make sure we can handle the method
    if (req.method() != http::verb::get && req.method() != http::verb::head) {
        return send(Response::bad_request(req));
    }

    // Request path must be absolute and not contain "..".
    if (req.target().empty() ||
        req.target()[0] != '/' ||
        req.target().find("..") != string_view::npos) {
        return send(Response::bad_request(req));
    }

    std::string path = path_cat(root, req.target());
    if (req.target().back() == '/') {
        path.append("index.html");
    }

    boost::beast::error_code code;
    http::file_body::value_type body;
    body.open(path.c_str(), boost::beast::file_mode::scan, code);

    if (code == boost::system::errc::no_such_file_or_directory) {
        return send(Response::not_found(req));
    }

    if (code) {
        return send(Response::internal_server_error(req));
    }

    if (req.method() == http::verb::head) {
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.set(http::field::server, string_param(BOOST_BEAST_VERSION_STRING));
        res.set(http::field::content_type, string_param(mime_type(path)));
        res.content_length(body.size());
        res.keep_alive(req.keep_alive());
        return send(std::move(res));
    }

    http::response<http::file_body> res{std::piecewise_construct, std::make_tuple(std::move(body)), std::make_tuple(http::status::ok, req.version())};
    res.set(http::field::server, string_param(BOOST_BEAST_VERSION_STRING));
    res.set(http::field::content_type, string_param(mime_type(path)));
    res.content_length(body.size());
    res.keep_alive(req.keep_alive());
    return send(std::move(res));
}

void server::Http::on_handshake(error_code code, size_t bytes_used) {
    if (code == operation_aborted) {
        return;
    }

    if (code) {
        return log("handshake", code);
    }

    _buffer.consume(bytes_used);
    read();
}

void server::Http::on_timer(error_code code) {
    if (code && code != operation_aborted) {
        return log("timer", code);
    }

    if (_timer.expiry() <= chrono::steady_clock::now()) {
        return timeout();
    }

    _timer.async_wait(
        bind_executor(
            _strand,
            bind(
                &Http::on_timer,
                shared_from_this(),
                placeholders::_1
            )
        )
    );
}

void server::Http::on_read(error_code code) {
    if (code == operation_aborted) {
        return;
    }

    if (code == http::error::end_of_stream) {
        return eof();
    }

    if (code) {
        return log("read", code);
    }

    if (websocket::is_upgrade(_req)) {
        make_shared<Websocket>(
            move(_socket),
            _secured,
            _ctx
        )->run(move(_req));
        _timer.expires_at(chrono::time_point<chrono::steady_clock>::max());
        return;
    }

    request_handler(_root, move(_req), _queue);

    if (!_queue.is_full()) {
        read();
    }
}

void server::Http::on_write(error_code code, bool close) {
    if (code == operation_aborted) {
        return;
    }

    if (code) {
        return log("write", code);
    }

    if (close) {
        return eof();
    }

    if (_queue.on_write()) {
        read();
    }
}

void server::Http::on_shutdown(error_code code) {
    if (code == operation_aborted) {
        return;
    }

    if (code) {
        return log("shutdown", code);
    }
}