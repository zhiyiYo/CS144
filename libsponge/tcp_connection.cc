#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _last_segment_time; }

void TCPConnection::segment_received(const TCPSegment &seg) {
    if (!active())
        return;

    _last_segment_time = 0;

    // 是否需要发送空包回复 ACK，比如没有数据的时候收到 SYN/ACK 也要回一个 ACK
    bool need_empty_ack = seg.length_in_sequence_space();

    auto &header = seg.header();

    // 处理 RST 标志位
    if (header.rst)
        return abort();

    // 将包交给发送者
    if (header.ack) {
        need_empty_ack |= !_sender.ack_received(header.ackno, header.win);

        // 队列中已经有数据报文段了就不需要专门的空包回复 ACK
        if (!_sender.segments_out().empty())
            need_empty_ack = false;
    }

    // 将包交给接受者
    need_empty_ack |= !_receiver.segment_received(seg);

    // 被动连接
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::SYN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::CLOSED)
        return connect();

    // 被动关闭
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::SYN_ACKED)
        _linger_after_streams_finish = false;

    // LAST_ACK 状态转移到 CLOSED
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED && !_linger_after_streams_finish) {
        _is_active = false;
        return;
    }

    if (need_empty_ack && TCPState::state_summary(_receiver) != TCPReceiverStateSummary::LISTEN)
        _sender.send_empty_segment();

    // 发送其余报文段
    send_segments();
}

bool TCPConnection::active() const { return _is_active; }

size_t TCPConnection::write(const string &data) {
    auto size = _sender.stream_in().write(data);
    send_segments(true);
    return size;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    _sender.tick(ms_since_last_tick);

    // 重传次数太多时需要断开连接
    if (_sender.consecutive_retransmissions() > _cfg.MAX_RETX_ATTEMPTS) {
        return send_rst_segment();
    }

    // 重传数据包
    send_segments();

    _last_segment_time += ms_since_last_tick;

    // 超时将状态转移到 CLOSED
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED &&
        _last_segment_time >= 10 * _cfg.rt_timeout) {
        _linger_after_streams_finish = false;
        _is_active = false;
    }
}

void TCPConnection::end_input_stream() {
    // 发送 FIN
    _sender.stream_in().end_input();
    send_segments(true);
}

void TCPConnection::send_segments(bool fill_window) {
    if (fill_window)
        _sender.fill_window();

    auto &segments = _sender.segments_out();


    while (!segments.empty()) {
        auto seg = segments.front();

        // 设置 ACK、确认应答号和接收窗口大小
        if (_receiver.ackno()) {
            seg.header().ackno = _receiver.ackno().value();
            seg.header().win = _receiver.window_size();
            seg.header().ack = true;
        }

        _segments_out.push(seg);
        segments.pop();
    }
}

void TCPConnection::send_rst_segment() {
    abort();
    TCPSegment seg;
    seg.header().rst = true;
    _segments_out.push(seg);
}

void TCPConnection::abort() {
    _is_active = false;
    _sender.stream_in().set_error();
    _receiver.stream_out().set_error();
}

void TCPConnection::connect() {
    // 发送 SYN
    send_segments(true);
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";

            // Your code here: need to send a RST segment to the peer
            send_rst_segment();
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
