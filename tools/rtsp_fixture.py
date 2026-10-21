"""Local-only synthetic RTSP fixture; requires gir1.2-gst-rtsp-server-1.0."""
import gi
gi.require_version('Gst','1.0')
gi.require_version('GstRtspServer','1.0')
from gi.repository import Gst, GstRtspServer, GLib
Gst.init(None)
server=GstRtspServer.RTSPServer()
server.set_address('127.0.0.1')
server.set_service('8554')
factory=GstRtspServer.RTSPMediaFactory()
factory.set_launch('( videotestsrc is-live=true pattern=ball ! video/x-raw,width=640,height=480,framerate=15/1 ! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast key-int-max=15 ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )')
factory.set_shared(True)
server.get_mount_points().add_factory('/test',factory)
if not server.attach(None):raise RuntimeError('cannot bind local RTSP server')
print('READY rtsp://127.0.0.1:8554/test',flush=True)
GLib.MainLoop().run()
