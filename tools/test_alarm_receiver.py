"""Receive boundary regressions: idle connections versus incomplete packets."""
import socket
import unittest
from alarm_receiver import receive_exact


class ReceiveTests(unittest.TestCase):
    def setUp(self):
        self.reader,self.writer=socket.socketpair()
        self.reader.settimeout(.05)
        self.addCleanup(self.reader.close)
        self.addCleanup(self.writer.close)

    def test_idle_header_is_normal_disconnect(self):
        self.assertIsNone(receive_exact(self.reader,4,idle_ok=True))

    def test_partial_header_timeout_still_fails(self):
        self.writer.sendall(b'\x00\x00')
        with self.assertRaises(TimeoutError):
            receive_exact(self.reader,4,idle_ok=True)

    def test_missing_body_timeout_still_fails(self):
        with self.assertRaises(TimeoutError):
            receive_exact(self.reader,7)

    def test_complete_header_and_body(self):
        self.writer.sendall(b'\x00\x00\x00\x03abc')
        self.assertEqual(receive_exact(self.reader,4,idle_ok=True),b'\x00\x00\x00\x03')
        self.assertEqual(receive_exact(self.reader,3),b'abc')

    def test_partial_packet_eof_still_fails(self):
        self.writer.sendall(b'\x00');self.writer.shutdown(socket.SHUT_WR)
        with self.assertRaises(EOFError):
            receive_exact(self.reader,4,idle_ok=True)


if __name__=='__main__':unittest.main()
