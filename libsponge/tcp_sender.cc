#include "tcp_sender.hh"

#include "tcp_config.hh"

#include<math.h>
#include <random>

// TCP 发送方的伪实现

// 对于实验 3，请用一个能通过 `make check_lab3` 自动检查的真实实现来替换它。

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] capacity 出站字节流的容量
//! \param[in] retx_timeout 重传最旧的未完成段之前的初始等待时间
//! \param[in] fixed_isn 要使用的初始序列号（如果已设置）（否则使用随机 ISN）
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _timeout(retx_timeout)
    , _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity) {}

uint64_t TCPSender::bytes_in_flight() const { return _flight_bytes; }

void TCPSender::fill_window() {
    auto cur_win_sz = std::max(1UL,_last_win_sz);
    while(true){
        TCPSegment segment;
        auto &header = segment.header();
        if(!_syn){
            header.syn = true;
            _syn = true;
        }
        header.seqno = next_seqno();
        auto payload_sz = std::min(TCPConfig::MAX_PAYLOAD_SIZE, cur_win_sz - _flight_bytes - header.syn);
        std::string payload = _stream.read(payload_sz);

        /**
        * 设置fin的条件
        * 1. 从没发过fin; fin会占用序列号导致bytes_in_flight非空, 前个fin包是会触发重传的，并不需要主动再次发fin
        * 2. 输入流eof
        * 3. fin占用一个序列号, 所以fin必须能放在窗口中
        */

        if(!_fin && _stream.eof() && payload.size() + _flight_bytes < cur_win_sz) {
            _fin = true;
            header.fin = true;
        }
        segment.payload() = std::move(payload);
        if(segment.length_in_sequence_space() == 0) break;
        if(_outgoing_queue.empty()){
            _timeout = _initial_retransmission_timeout;
            _time_wait = 0;
        }
        _segments_out.push(segment);
        auto len = segment.length_in_sequence_space();
        _flight_bytes += len;
        _outgoing_queue.push({_next_seqno,segment});
        _next_seqno += len;
        if(header.fin){
            break;
        }
    }
}

//! \param ackno 远程接收方的 ackno (确认号)
//! \param window_size 远程接收方通告的窗口大小
//! \returns 如果 ackno 看起来无效（确认了 TCPSender 尚未发送的内容），则返回 `false`
bool TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    auto abs_seqno = unwrap(ackno, _isn, _next_seqno);
    // ackno 无效
    if (abs_seqno > _next_seqno) {
        return false;
    }
    for (; !_outgoing_queue.empty();) {
        const auto &front = _outgoing_queue.front();
        const auto &segment = front.second;
        if (front.first + segment.length_in_sequence_space() <= abs_seqno) {
        _flight_bytes -= segment.length_in_sequence_space();
        _outgoing_queue.pop();
        // 新的数据包被接受，清空超时时间
        _timeout = _initial_retransmission_timeout;
        _time_wait = 0;
        } else {
        // 当前数据包未被确认, 那么之后的数据包不用检查
        break;
        }
    }
    _consecutive_retransmissions_count = 0;
    _last_win_sz = window_size;
    fill_window();
    return true;
}

//! \param[in] ms_since_last_tick 距离上次调用此方法以来经过的毫秒数
void TCPSender::tick(const size_t ms_since_last_tick) { 
    _time_wait += ms_since_last_tick;
    if (_outgoing_queue.empty()) {
        return;
    }
    if (_time_wait >= _timeout) {
        const auto &front = _outgoing_queue.front();
        // 窗口大小不为0
        if (_last_win_sz > 0) {
            _timeout *= 2;
        }
        _time_wait = 0;
        // 再次发送
        _segments_out.push(front.second);
        // 连续重传次数加1
        ++_consecutive_retransmissions_count;
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions_count; }

void TCPSender::send_empty_segment() {
    TCPSegment segment;
    segment.header().seqno = next_seqno();
    _segments_out.push(segment);
}
