#include "connection.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace cdp::detail {

    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    using tcp = boost::asio::ip::tcp;

    struct Connection::Impl {
        boost::asio::io_context io;
        std::optional<websocket::stream<tcp::socket>> ws;
        bool connected = false;
    };

    Connection::Connection() : impl(std::make_unique<Impl>()) {}
    Connection::~Connection() = default;
    Connection::Connection(Connection&&) noexcept = default;
    Connection& Connection::operator=(Connection&&) noexcept = default;

    void Connection::connect(const std::string& host,
        const std::string& port,
        const std::string& target)
    {
        impl->ws.emplace(impl->io);
        tcp::resolver resolver(impl->io);
        auto results = resolver.resolve(host, port);
        boost::asio::connect(impl->ws->next_layer(), results);
        impl->ws->handshake(host + ":" + port, target);
        impl->connected = true;
    }

    void Connection::send(const std::string& message) {
        if (impl->ws) impl->ws->write(boost::asio::buffer(message));
    }

    std::string Connection::receive(std::chrono::milliseconds timeout) {
        if (!impl->ws) return {};

        beast::flat_buffer buffer;
        beast::error_code  read_ec;
        bool               completed = false;


        impl->io.restart();
        impl->ws->async_read(buffer,
            [&](beast::error_code ec, std::size_t /*bytes*/) {
                read_ec = ec;
                completed = true;
            });

        impl->io.run_for(timeout);

        if (!completed) {

            beast::error_code ignore;
            impl->ws->next_layer().cancel(ignore);
            impl->io.restart();
            impl->io.run();                 
            throw TimeoutError("websocket read timed out");
        }

        if (read_ec == websocket::error::closed) {
            impl->connected = false;
            return {};                       
        }
        if (read_ec) {
            impl->connected = false;
            throw std::runtime_error("websocket read error: " + read_ec.message());
        }

        return beast::buffers_to_string(buffer.data());
    }

    std::string Connection::read() {
        if (!impl->ws) return {};

        beast::flat_buffer buffer;
        beast::error_code  ec;
        impl->ws->read(buffer, ec);

        if (ec) {
            impl->connected = false;
            if (ec == websocket::error::closed) return {};
            if (ec == boost::asio::error::operation_aborted) return {};
            if (ec == boost::asio::error::bad_descriptor) return {};
            if (ec == boost::asio::error::connection_aborted) return {};
            if (ec == boost::asio::error::connection_reset) return {};
            throw std::runtime_error("websocket read error: " + ec.message());
        }

        return beast::buffers_to_string(buffer.data());
    }

    void Connection::close() {
        if (impl->ws) {
            beast::error_code ec;
            impl->ws->next_layer().close(ec);
        }
        impl->ws.reset();
        impl->connected = false;
    }

    bool Connection::is_connected() const {
        return impl->connected;
    }

} // namespace cdp::detail
