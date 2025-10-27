#ifndef SPONGE_LIBSPONGE_TCP_SENDER_HH
#define SPONGE_LIBSPONGE_TCP_SENDER_HH

#include "byte_stream.hh"
#include "tcp_config.hh"
#include "tcp_segment.hh"
#include "wrapping_integers.hh"

#include <functional>
#include <queue>

//! \brief TCP 实现的“发送方”部分。

//! 接受一个 ByteStream，将其分割成段并发送，
//! 跟踪哪些段仍在传输中，
//! 维护重传计时器，并在计时器到期时重传传输中的段。
class TCPSender {
  private:
    uint32_t _timeout;
    uint32_t _time_wait{0};

    std::queue<std::pair<size_t,TCPSegment>> _outgoing_queue{};
    uint64_t _flight_bytes{0};

    size_t _last_win_sz{1};
    bool _syn{false};
    bool _fin{false};
    size_t _consecutive_retransmissions_count{0};

    //! 我们的初始序列号，即 SYN 的序列号。
    WrappingInt32 _isn;

    //! TCPSender 希望发送的段的出站队列
    std::queue<TCPSegment> _segments_out{};
    //! 连接的重传计时器
    unsigned int _initial_retransmission_timeout;

    //! 尚未发送的字节的出站流
    ByteStream _stream;

    //! 下一个要发送的字节的（绝对）序列号
    uint64_t _next_seqno{0};

  public:
    //! 初始化一个 TCPSender
    TCPSender(const size_t capacity = TCPConfig::DEFAULT_CAPACITY,
              const uint16_t retx_timeout = TCPConfig::TIMEOUT_DFLT,
              const std::optional<WrappingInt32> fixed_isn = {});

    //! \name 作者的“输入”接口
    //!@{
    ByteStream &stream_in() { return _stream; }
    const ByteStream &stream_in() const { return _stream; }
    //!@}

    //! \name 可能导致 TCPSender 发送段的方法
    //!@{

    //! \brief 收到了一个新的确认
    bool ack_received(const WrappingInt32 ackno, const uint16_t window_size);

    //! \brief 生成一个空载荷的段（用于创建空的 ACK 段）
    void send_empty_segment();

    //! \brief 创建并发送段以尽可能填满窗口
    void fill_window();

    //! \brief 通知 TCPSender 时间的流逝
    void tick(const size_t ms_since_last_tick);
    //!@}

    //! \name 访问器
    //!@{

    //! \brief 有多少序列号被已发送但尚未确认的段占用？
    //! \note 计数在“序列号空间”中，即 SYN 和 FIN 各计为一个字节
    //! (参见 TCPSegment::length_in_sequence_space())
    size_t bytes_in_flight() const;

    //! \brief 连续发生的重传次数
    unsigned int consecutive_retransmissions() const;

    //! \brief TCPSender 已排队等待传输的 TCPSegments。
    //! \note 这些必须由 TCPConnection 出队并发送，
    //! 在发送前需要填写由 TCPReceiver 设置的字段（ackno 和 window size）。
    std::queue<TCPSegment> &segments_out() { return _segments_out; }
    //!@}

    //! \name 下一个序列号是什么？ (用于测试)
    //!@{

    //! \brief 下一个要发送的字节的绝对序列号
    uint64_t next_seqno_absolute() const { return _next_seqno; }

    //! \brief 下一个要发送的字节的相对序列号
    WrappingInt32 next_seqno() const { return wrap(_next_seqno, _isn); }
    //!@}
};

#endif  // SPONGE_LIBSPONGE_TCP_SENDER_HH
