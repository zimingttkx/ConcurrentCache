#include "connection.h"

namespace cc_server {
    Buffer* Connection::input_buffer() {
        return &input_buffer_;
    }

    Buffer* Connection::output_buffer() {
        return &output_buffer_;
    }

    int Connection::fd() const {
        return client_socket_.fd();
    }
}
