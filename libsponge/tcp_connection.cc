#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::segment_received(const TCPSegment &seg) { 
  _time_since_last_segment_received = 0;
  bool need_send_ack = seg.length_in_sequence_space() != 0;

  auto &header = seg.header();
  // 收到rst包, 则直接终止
  if (header.rst) {
    set_rst_state(false);
    return;
  }
  // 接收数据
  _receiver.segment_received(seg);

  if (header.ack) {
    _sender.ack_received(header.ackno, header.win);
  }

  if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::SYN_RECV &&
      TCPState::state_summary(_sender) == TCPSenderStateSummary::CLOSED) {
    // 发送SYN+ACK
    connect();
    return;
  }

  if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
      TCPState::state_summary(_sender) == TCPSenderStateSummary::SYN_ACKED) {
    // 直接断开连接，不需要等待
    _linger_after_streams_finish = false;
  }

  if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
      TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED && !_linger_after_streams_finish) {
    _is_active = false;
    return;
  }

  bool keepalive = (seg.length_in_sequence_space() == 0 && _receiver.ackno().has_value() &&
                    _receiver.ackno().value() != header.seqno);
  keepalive |= (header.ack && header.ackno - _sender.next_seqno() > 0);
  if (!need_send_ack && keepalive) {
    // 没有建立连接，不需要发送空的ack
    if (TCPState::state_summary(_receiver) != TCPReceiverStateSummary::SYN_RECV ||
        TCPState::state_summary(_sender) != TCPSenderStateSummary::SYN_ACKED) {
      keepalive = false;
    }
  }
  if (need_send_ack || keepalive) {
    _sender.send_empty_segment();
  }
  // 给待发送的包添加 ack和win
  attach_with_ack_and_win();
 }

bool TCPConnection::active() const { return _is_active; }

size_t TCPConnection::write(const string &data) {
    auto write_count = _sender.stream_in().write(data);
    _sender.fill_window();
    // 给待发送的包添加 ack和win
    attach_with_ack_and_win();
    return write_count;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) { 
    _sender.tick(ms_since_last_tick);
  if (_sender.consecutive_retransmissions() > _cfg.MAX_RETX_ATTEMPTS) {
    // 重传次数过多
    set_rst_state(true);
    return;
  }
  // 给待发送的包添加 ack和win
  attach_with_ack_and_win();

  _time_since_last_segment_received += ms_since_last_tick;

  if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
      TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED && _linger_after_streams_finish &&
      _time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
    _is_active = false;
    _linger_after_streams_finish = false;
  }
 }

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    
    _sender.fill_window();

    attach_with_ack_and_win();
}

void TCPConnection::connect() {
    _sender.fill_window();
    _is_active = true;
    // 给待发送的包添加 ack和win
    attach_with_ack_and_win();
}

void TCPConnection::set_rst_state(bool send_rst){
    if(send_rst) {
        TCPSegment segment;
        segment.header().rst = true;
        _segments_out.push(segment);
    }

    _receiver.stream_out().set_error();
    _sender.stream_in().set_error();
    _linger_after_streams_finish = false;
    _is_active = false;
}

void TCPConnection::attach_with_ack_and_win() {
  // 为数据包添加ack和win
  while (!_sender.segments_out().empty()) {
    auto &segment = _sender.segments_out().front();
    auto &header = segment.header();
    // 处于SYN_RECV之后的状态
    if (_receiver.ackno().has_value()) {
      header.ack = true;
      // 添加 确认号+窗口大小
      header.ackno = _receiver.ackno().value();
      header.win = _receiver.window_size();
    }
    _segments_out.push(segment);
    _sender.segments_out().pop();
  }
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";
            
            // Your code here: need to send a RST segment to the peer
            set_rst_state(false);
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
