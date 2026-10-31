#!/usr/bin/env python3
"""Generate golden binary FIX fixtures with real SOH (0x01) delimiters.

Release gate A asks for golden BINARY fixtures, meaning files that contain byte
0x01 rather than the vertical bar used when printing examples. BodyLength and
CheckSum are computed here, in a third implementation independent of both
src/codec.cpp and tests/support/peer_wire.hpp, so a mistake has to be made three
times in three languages to go unnoticed.

Each fixture is listed in fixtures/manifest.tsv with the outcome the engine must
produce, and tests/unit/test_golden_fixtures.cpp asserts exactly that.
"""

import os

SOH = b"\x01"
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fixtures")


def frame(fields, begin_string=b"FIX.4.4", body_length=None, checksum=None):
    """Assemble a frame. body_length and checksum override the computed values,
    which is how the deliberately broken fixtures are produced."""
    body = b"".join(b"%d=%s%s" % (tag, value, SOH) for tag, value in fields)
    length = len(body) if body_length is None else body_length
    head = b"8=" + begin_string + SOH + b"9=" + str(length).encode() + SOH
    without_trailer = head + body
    if checksum is None:
        checksum = b"%03d" % (sum(without_trailer) % 256)
    return without_trailer + b"10=" + checksum + SOH


def header(msg_type, seq, sender=b"SIMLP01", target=b"TRADER01",
           sending_time=b"20261030-05:00:00.000"):
    return [(35, msg_type), (49, sender), (56, target), (34, str(seq).encode()),
            (52, sending_time)]


def write(relative, data, expectation, error, note):
    path = os.path.join(ROOT, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)
    return (relative.replace(os.sep, "/"), expectation, error, note)


def valid_fixtures():
    rows = []
    rows.append(write("valid/logon.fix",
                      frame(header(b"A", 1, sender=b"TRADER01", target=b"SIMLP01") +
                            [(98, b"0"), (108, b"30")]),
                      "ACCEPT", "None", "a well formed Logon"))
    rows.append(write("valid/heartbeat.fix", frame(header(b"0", 2)),
                      "ACCEPT", "None", "a bare heartbeat"))
    rows.append(write("valid/heartbeat_with_testreqid.fix",
                      frame(header(b"0", 3) + [(112, b"probe-1")]),
                      "ACCEPT", "None", "a heartbeat answering a TestRequest"))
    rows.append(write("valid/test_request.fix",
                      frame(header(b"1", 4) + [(112, b"probe-2")]),
                      "ACCEPT", "None", "a TestRequest"))
    rows.append(write("valid/resend_request.fix",
                      frame(header(b"2", 5) + [(7, b"10"), (16, b"20")]),
                      "ACCEPT", "None", "a bounded ResendRequest"))
    rows.append(write("valid/resend_request_open_ended.fix",
                      frame(header(b"2", 6) + [(7, b"10"), (16, b"0")]),
                      "ACCEPT", "None", "an open ended ResendRequest"))
    rows.append(write("valid/sequence_reset_gapfill.fix",
                      frame(header(b"4", 7) + [(43, b"Y"), (122, b"20261030-04:59:00.000"),
                                               (123, b"Y"), (36, b"12")]),
                      "ACCEPT", "None", "a SequenceReset-GapFill"))
    rows.append(write("valid/logout.fix", frame(header(b"5", 8) + [(58, b"end of day")]),
                      "ACCEPT", "None", "a Logout with text"))
    rows.append(write("valid/reject.fix",
                      frame(header(b"3", 9) + [(45, b"7"), (373, b"1"), (58, b"missing tag")]),
                      "ACCEPT", "None", "a session level Reject"))
    rows.append(write("valid/app_message.fix",
                      frame(header(b"U1", 10) + [(20001, b"payload")]),
                      "ACCEPT", "None", "an application message from the test dictionary"))
    rows.append(write("valid/timestamp_no_fraction.fix",
                      frame(header(b"0", 11, sending_time=b"20261030-05:00:00")),
                      "ACCEPT", "None", "second precision SendingTime"))
    rows.append(write("valid/timestamp_nanoseconds.fix",
                      frame(header(b"0", 12, sending_time=b"20261030-05:00:00.123456789")),
                      "ACCEPT", "None", "nanosecond precision SendingTime"))
    rows.append(write("valid/leap_day.fix",
                      frame(header(b"0", 13, sending_time=b"20280229-05:00:00.000")),
                      "ACCEPT", "None", "a real leap day"))
    return rows


def framing_error_fixtures():
    rows = []
    app_fields = header(b"U1", 10) + [(20001, b"payload")]
    body_len = len(b"".join(b"%d=%s%s" % (t, v, SOH) for t, v in app_fields))

    rows.append(write("invalid/body_length_short.fix",
                      frame(app_fields, body_length=body_len - 20),
                      "FRAMING_ERROR", "BodyLengthMismatch",
                      "BodyLength smaller than the actual body"))
    rows.append(write("invalid/body_length_alpha.fix",
                      frame(app_fields).replace(b"9=" + str(body_len).encode(), b"9=abc", 1),
                      "FRAMING_ERROR", "BadBodyLength", "a non numeric BodyLength"))
    rows.append(write("invalid/body_length_overflow.fix",
                      frame(app_fields, body_length=99999999999999999999),
                      "FRAMING_ERROR", "BodyLengthOverflow", "a twenty digit BodyLength"))
    rows.append(write("invalid/body_length_too_large.fix",
                      frame(app_fields, body_length=100000000),
                      "FRAMING_ERROR", "BodyLengthTooLarge",
                      "BodyLength beyond the configured frame cap"))
    rows.append(write("invalid/checksum_wrong.fix", frame(app_fields, checksum=b"000"),
                      "FRAMING_ERROR", "CheckSumMismatch", "a checksum that does not match"))
    rows.append(write("invalid/checksum_two_digits.fix", frame(app_fields, checksum=b"12"),
                      "FRAMING_ERROR", "BadCheckSumFormat", "a two digit checksum"))
    rows.append(write("invalid/no_begin_string.fix", frame(header(b"U1", 10))[10:],
                      "FRAMING_ERROR", "MissingBeginString",
                      "the frame does not start with tag 8"))
    rows.append(write("invalid/pipe_delimiters.fix",
                      frame(header(b"U1", 10)).replace(SOH, b"|"),
                      "FRAMING_ERROR", "Incomplete",
                      "vertical bars instead of SOH: never a valid frame"))
    return rows


def decode_error_fixtures():
    rows = []
    rows.append(write("invalid/wrong_version.fix",
                      frame(header(b"0", 1), begin_string=b"FIX.4.2"),
                      "DECODE_ERROR", "BadBeginString",
                      "a self consistent FIX.4.2 frame: only the version is wrong"))
    rows.append(write("invalid/missing_seqnum.fix",
                      frame([(35, b"0"), (49, b"SIMLP01"), (56, b"TRADER01"),
                             (52, b"20261030-05:00:00.000")]),
                      "DECODE_ERROR", "MissingRequiredHeader", "no MsgSeqNum"))
    rows.append(write("invalid/missing_sending_time.fix",
                      frame([(35, b"0"), (49, b"SIMLP01"), (56, b"TRADER01"), (34, b"1")]),
                      "DECODE_ERROR", "MissingRequiredHeader", "no SendingTime"))
    rows.append(write("invalid/duplicate_seqnum.fix",
                      frame(header(b"0", 1) + [(34, b"2")]),
                      "DECODE_ERROR", "DuplicateHeaderTag", "MsgSeqNum appears twice"))
    rows.append(write("invalid/msgtype_not_third.fix",
                      frame([(49, b"SIMLP01"), (35, b"0"), (56, b"TRADER01"), (34, b"1"),
                             (52, b"20261030-05:00:00.000")]),
                      "DECODE_ERROR", "MissingMsgType", "MsgType outside the third position"))
    rows.append(write("invalid/bad_timestamp.fix",
                      frame(header(b"0", 1, sending_time=b"20270229-05:00:00.000")),
                      "DECODE_ERROR", "BadTimestamp", "2027 is not a leap year"))
    rows.append(write("invalid/possdup_without_origsendingtime.fix",
                      frame(header(b"U1", 1) + [(43, b"Y"), (20001, b"x")]),
                      "DECODE_ERROR", "MissingRequiredHeader",
                      "PossDupFlag with no OrigSendingTime"))
    rows.append(write("invalid/possdup_not_boolean.fix",
                      frame(header(b"U1", 1) + [(43, b"MAYBE"),
                                                (122, b"20261030-04:00:00.000"),
                                                (20001, b"x")]),
                      "DECODE_ERROR", "BadFieldValue", "PossDupFlag is neither Y nor N"))
    rows.append(write("invalid/zero_seqnum.fix", frame(header(b"0", 0)),
                      "DECODE_ERROR", "BadSeqNum", "MsgSeqNum is zero"))
    rows.append(write("invalid/empty_value.fix", frame(header(b"0", 1) + [(112, b"")]),
                      "DECODE_ERROR", "EmptyTagValue", "a tag with no value"))
    rows.append(write("invalid/non_ascii_value.fix",
                      frame(header(b"U1", 1) + [(20001, b"ok\xc3\xa9")]),
                      "DECODE_ERROR", "NonPrintableValue", "a non ASCII byte in a value"))
    return rows


def stream_fixtures():
    rows = []
    rows.append(write("gap/future_pair.fix",
                      frame(header(b"U1", 102) + [(20001, b"first-seen")]) +
                      frame(header(b"U1", 104) + [(20001, b"also-ahead")]),
                      "STREAM", "",
                      "two messages ahead of the expected sequence, 101 and 103 missing"))
    rows.append(write("gap/fill_pair.fix",
                      frame(header(b"U1", 101) + [(20001, b"fills-first")]) +
                      frame(header(b"U1", 103) + [(20001, b"fills-second")]),
                      "STREAM", "", "the two messages that close the gap above"))
    rows.append(write("retransmission/app_possdup.fix",
                      frame([(35, b"U1"), (49, b"SIMLP01"), (56, b"TRADER01"), (34, b"101"),
                             (52, b"20261030-06:00:00.000"), (43, b"Y"),
                             (122, b"20261030-05:00:00.000"), (20001, b"replayed")]),
                      "ACCEPT", "None",
                      "a retransmission: original sequence, PossDupFlag, OrigSendingTime"))
    rows.append(write("retransmission/gapfill_over_admin.fix",
                      frame([(35, b"4"), (49, b"SIMLP01"), (56, b"TRADER01"), (34, b"11"),
                             (52, b"20261030-06:00:00.000"), (43, b"Y"),
                             (122, b"20261030-05:00:00.000"), (123, b"Y"), (36, b"13")]),
                      "ACCEPT", "None", "a GapFill covering sequences 11 and 12"))
    return rows


def main():
    rows = valid_fixtures() + framing_error_fixtures() + decode_error_fixtures() + \
        stream_fixtures()
    manifest = os.path.join(ROOT, "manifest.tsv")
    with open(manifest, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# path\texpectation\texpected_error\tnote\n")
        for path, expectation, error, note in rows:
            handle.write(f"{path}\t{expectation}\t{error}\t{note}\n")
    print(f"wrote {len(rows)} fixtures and {manifest}")


if __name__ == "__main__":
    main()
