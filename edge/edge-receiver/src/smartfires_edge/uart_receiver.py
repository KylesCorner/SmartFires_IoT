import datetime
import struct
from typing import Optional

import serial
from smartfires_edge.packet import (
    BASE_FRAME_MAX_DATA_LEN,
    BASE_FRAME_MIN_DATA_LEN,
    PKT_CALIBRATION_DATA,
    PKT_CMD_ACK,
    FRAME_M0,
    FRAME_M1,
    HEADER_FMT,
    PKT_BUNDLE,
    PKT_FULL_STATE,
    PKT_GPS,
    PKT_MAGIC,
    PKT_STATUS,
    crc8,
    decode_calibration_data,
    decode_bundle,
    decode_cmd_ack,
    decode_full_state,
    decode_gps,
    decode_status,
)

_ST_WAIT_M0 = 0
_ST_WAIT_M1 = 1
_ST_WAIT_LEN = 2
_ST_READ_DATA = 3
_ST_CHECK_CRC = 4


class FrameReceiver:
    def __init__(self) -> None:
        self.state = _ST_WAIT_M0
        self.buf = bytearray()
        self.expected_len = 0
        self.crc_failures = 0
        self.length_failures = 0

    def push_byte(self, b: int) -> Optional[dict]:
        if self.state == _ST_WAIT_M0:
            if b == FRAME_M0:
                self.state = _ST_WAIT_M1
            return None

        if self.state == _ST_WAIT_M1:
            self.state = _ST_WAIT_LEN if b == FRAME_M1 else _ST_WAIT_M0
            return None

        if self.state == _ST_WAIT_LEN:
            self.expected_len = b
            if self.expected_len < BASE_FRAME_MIN_DATA_LEN or self.expected_len > BASE_FRAME_MAX_DATA_LEN:
                self.length_failures += 1
                self.state = _ST_WAIT_M0
                return None
            self.buf = bytearray([b])
            self.state = _ST_READ_DATA
            return None

        if self.state == _ST_READ_DATA:
            self.buf.append(b)
            if len(self.buf) == 1 + self.expected_len:
                self.state = _ST_CHECK_CRC
            return None

        if self.state == _ST_CHECK_CRC:
            received_crc = b
            computed_crc = crc8(bytes(self.buf))
            self.state = _ST_WAIT_M0

            if received_crc != computed_crc:
                self.crc_failures += 1
                self.buf = bytearray()
                return None

            rssi = struct.unpack_from("<b", self.buf, 1)[0]
            raw_payload = bytes(self.buf[2:])
            self.buf = bytearray()

            pkt_type = raw_payload[1] if len(raw_payload) >= 2 else 0xFF
            hdr_node = None
            hdr_seq = None
            if len(raw_payload) >= 4:
                magic, _hdr_pkt, node_id, seq = struct.unpack_from(HEADER_FMT, raw_payload, 0)
                if magic == PKT_MAGIC:
                    hdr_node = node_id
                    hdr_seq = seq

            gps = None
            status = None
            calibration_data = None
            cmd_ack = None
            packets: list[dict] = []
            if pkt_type == PKT_GPS or pkt_type == PKT_STATUS:
                gps = decode_gps(raw_payload, rssi)
                status = decode_status(raw_payload, rssi)
            elif pkt_type == PKT_FULL_STATE:
                pkt = decode_full_state(raw_payload, rssi)
                if pkt is not None:
                    packets = [pkt]
            elif pkt_type == PKT_BUNDLE:
                packets = decode_bundle(raw_payload, rssi)
            elif pkt_type == PKT_CALIBRATION_DATA:
                calibration_data = decode_calibration_data(raw_payload, rssi)
            elif pkt_type == PKT_CMD_ACK:
                cmd_ack = decode_cmd_ack(raw_payload, rssi)

            now_ts = datetime.datetime.utcnow().isoformat(timespec="milliseconds")
            for pkt in packets:
                pkt["timestamp"] = now_ts

            return {
                "pkt_type": pkt_type,
                "node_id": hdr_node,
                "seq": hdr_seq,
                "rssi": rssi,
                "gps": gps,
                "status": status,
                "calibration_data": calibration_data,
                "cmd_ack": cmd_ack,
                "packets": packets,
            }

        self.state = _ST_WAIT_M0
        self.buf = bytearray()
        return None


def iter_packets(port: str, baud: int):
    receiver = FrameReceiver()

    with serial.Serial(port, baud, timeout=0.25) as ser:
        while True:
            raw = ser.read(1)
            if not raw:
                continue

            event = receiver.push_byte(raw[0])
            if event is not None:
                yield event, receiver, ser
