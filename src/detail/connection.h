#pragma once

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace cdp::detail {

    struct TimeoutError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    class Connection {
    public:
        Connection();
        ~Connection();                             

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept;          
        Connection& operator=(Connection&&) noexcept;
        std::string read();
        void connect(const std::string& host, const std::string& port, const std::string& target);
        void send(const std::string& message);


        std::string receive(std::chrono::milliseconds timeout);

        bool is_connected() const;
        void close();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;   
    };

} // namespace cdp::detail
