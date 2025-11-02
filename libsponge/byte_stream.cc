#include "byte_stream.hh"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include<math.h>

// Dummy implementation of a flow-controlled in-memory byte stream.

// For Lab 0, please replace with a real implementation that passes the
// automated checks run by `make check_lab0`.

// You will need to add private members to the class declaration in `byte_stream.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;
ByteStream::ByteStream(const size_t capacity) : _capacity(capacity) {}

size_t ByteStream::write(const string &data) {
    if (_input_end) {
    return 0;
  }
  size_t remain = _capacity - _buf.size();
  size_t write_count = std::min(remain, data.size());
  _write_bytes += write_count;
  _buf.insert(_buf.end(), data.begin(), data.begin() + write_count);
  return write_count;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    size_t read_count = std::min(len, _buf.size());
    return std::string(_buf.begin(), _buf.begin() + read_count);
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) {
  size_t pop_count = std::min(len, _buf.size());
  _read_bytes += pop_count;
  _buf.erase(_buf.begin(), _buf.begin() + pop_count);
}

void ByteStream::end_input() { _input_end = true; }

bool ByteStream::input_ended() const { return _input_end; }

size_t ByteStream::buffer_size() const { return _buf.size(); }

bool ByteStream::buffer_empty() const { return _buf.size() == 0; }

bool ByteStream::eof() const { return buffer_empty() && input_ended(); }

size_t ByteStream::bytes_written() const { return _write_bytes; }

size_t ByteStream::bytes_read() const { return _read_bytes; }

size_t ByteStream::remaining_capacity() const { return _capacity - _buf.size(); }
