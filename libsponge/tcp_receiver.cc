#include "tcp_receiver.hh"

// Dummy implementation of a TCP receiver

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

bool TCPReceiver::segment_received(const TCPSegment &seg) {
    auto &header = seg.header();

    // 在完成握手之前不能接收数据
    if (!_is_syned && !header.syn)
        return false;

    // 丢弃网络延迟导致的重复 FIN
    if(_reassembler.input_ended() && header.fin)
        return false;

    // SYN
    if (header.syn) {

        // 丢弃网络延迟导致的重复 SYN
        if (_is_syned)
            return false;

        _isn = header.seqno;
        _is_syned = true;

        // FIN
        if (header.fin)
            _reassembler.push_substring(seg.payload().copy(), 0, true);

        return true;
    }

    // 分段所占的序列号长度
    size_t seg_len = max(seg.length_in_sequence_space(), 1UL);

    // 将序列号转换为字节流索引
    _checkpoint = unwrap(header.seqno, _isn, _checkpoint);
    uint64_t index = _checkpoint - 1;

    // 窗口右边界
    uint64_t unaccept_index = max(window_size(), 1UL) + _reassembler.next_index();

    // 序列号不能落在窗口外
    if (seg_len + index <= _reassembler.next_index() || index >= unaccept_index)
        return false;

    // 保存数据
    _reassembler.push_substring(seg.payload().copy(), index, header.fin);
    return true;
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    if (!_is_syned)
        return nullopt;

    return {wrap(_reassembler.next_index() + 1 + _reassembler.input_ended(), _isn)};
}

size_t TCPReceiver::window_size() const { return _reassembler.stream_out().remaining_capacity(); }
