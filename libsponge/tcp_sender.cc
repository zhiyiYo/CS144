#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <iostream>
#include <random>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity)
    , _timer(retx_timeout) {}

uint64_t TCPSender::bytes_in_flight() const { return _outstand_bytes; }

void TCPSender::fill_window() {
    if (!_is_syned) {
        // 等待 SYN 超时
        if (!_outstand_segments.empty())
            return;

        // 发送一个 SYN 包
        send_segment("", true);
    } else {
        size_t remain_size = max(_window_size, static_cast<uint16_t>(1)) + _ack_seq - _next_seqno;

        // 当缓冲区中有待发送数据时就发送数据报文段
        while (remain_size > 0 && !_stream.buffer_empty()) {
            auto ws = min(min(remain_size, TCPConfig::MAX_PAYLOAD_SIZE), _stream.buffer_size());
            remain_size -= ws;

            string &&data = _stream.peek_output(ws);
            _stream.pop_output(ws);

            // 置位 FIN
            _is_fin |= (_stream.eof() && !_is_fin && remain_size > 0);
            send_segment(std::move(data), false, _is_fin);
        }

        // 缓冲区输入结束时发送 FIN（缓冲区为空时不会进入循环体，需要再次发送）
        if (_stream.eof() && !_is_fin && remain_size > 0) {
            _is_fin = true;
            send_segment("", false, true);
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
//! \returns `false` if the ackno appears invalid (acknowledges something the TCPSender hasn't sent yet)
bool TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    auto ack_seq = unwrap(ackno, _isn, _ack_seq);

    // absolute ackno 不能落在窗口外
    if (ack_seq > _next_seqno)
        return false;

    _window_size = window_size;

    // 忽略已处理过的确认应答号
    if (ack_seq <= _ack_seq)
        return true;

    _ack_seq = ack_seq;
    _is_syned = true;

    // 重置超时时间为初始值
    _timer.set_time_out(_initial_retransmission_timeout);
    _consecutive_retxs = 0;

    // 移除已被确认的报文段
    while (!_outstand_segments.empty()) {
        auto &[segment, seqno] = _outstand_segments.front();
        if (seqno >= ack_seq)
            break;

        _outstand_bytes -= segment.length_in_sequence_space();
        _outstand_segments.pop();
    }

    // 再次填满发送窗口
    fill_window();

    // 如果还有没被确认的报文段就重启计时器
    if (!_outstand_segments.empty())
        _timer.start();
    else
        _timer.stop();

    return true;
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    _timer.elapse(ms_since_last_tick);

    if (!_timer.is_time_out())
        return;

    if (_outstand_segments.empty()) {
        return _timer.set_time_out(_initial_retransmission_timeout);
    }

    // 超时需要重发第一个报文段，同时将超时时间翻倍
    _segments_out.push(_outstand_segments.front().first);

    _consecutive_retxs += 1;
    _timer.set_time_out(_initial_retransmission_timeout * (1 << _consecutive_retxs));
    _timer.start();
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retxs; }

void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = next_seqno();
    _segments_out.push(seg);
}

void TCPSender::send_segment(string &&data, bool syn, bool fin) {
    TCPSegment segment;
    segment.header().syn = syn;
    segment.header().fin = fin;
    segment.header().seqno = next_seqno();
    segment.payload() = std::move(data);

    _segments_out.push(segment);
    _outstand_segments.push({segment, _next_seqno});

    auto len = segment.length_in_sequence_space();
    _outstand_bytes += len;
    _next_seqno += len;
}

Timer::Timer(uint32_t rto) : _rto(rto), _remain_time(rto), _is_running(false) {}

void Timer::start() {
    _is_running = true;
    _remain_time = _rto;
}

void Timer::stop() { _is_running = false; }

bool Timer::is_time_out() { return _remain_time == 0; }

void Timer::elapse(size_t elapsed) {
    if (elapsed > _remain_time) {
        _remain_time = 0;
    } else {
        _remain_time -= elapsed;
    }
}

void Timer::set_time_out(uint32_t duration) {
    _rto = duration;
    _remain_time = duration;
}
