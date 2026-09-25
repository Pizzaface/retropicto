"""PlatformIO pre-build hook: keep Wi-Fi secrets out of flags and build logs."""
Import("env")
import json
import ipaddress
from pathlib import Path

root = Path(env.subst("$PROJECT_DIR"))
config = root / "wifi.local.json"
if not config.exists():
    raise RuntimeError("Create wifi.local.json from wifi.example.json before building Wi-Fi firmware")
try:
    values = json.loads(config.read_text(encoding="utf-8"))
    diagnostic_channel = values.get("diagnostic_channel", 0)
    if type(diagnostic_channel) is not int or not 0 <= diagnostic_channel <= 13:
        raise ValueError()
    wifi_channel = values.get("wifi_channel", 0)
    if type(wifi_channel) is not int or not 0 <= wifi_channel <= 13:
        raise ValueError()
    peer_ip = str(ipaddress.IPv4Address(values.get("node_a_ip", "0.0.0.0")))
    ssid, password = values["ssid"], values["password"]
    if not isinstance(ssid,str) or not isinstance(password,str):
        raise ValueError()
    if not 1 <= len(ssid.encode()) <= 32 or not 8 <= len(password.encode()) <= 63:
        raise ValueError()
    if "\0" in ssid or "\0" in password:
        raise ValueError()
    node_a = bytes.fromhex(values["node_a_mac"].replace(":", ""))
    node_b = bytes.fromhex(values["node_b_mac"].replace(":", ""))
    if len(node_a) != 6 or len(node_b) != 6 or node_a == node_b:
        raise ValueError()
except (ValueError, KeyError, TypeError, AttributeError):
    raise RuntimeError("wifi.local.json needs an SSID, WPA2 password (8..63 UTF-8 bytes), and two distinct node MACs") from None
peer_define = "#define ONLINE_NODE_A_IP " + json.dumps(peer_ip) + "\n"
header = root / "firmware" / "esp32" / "wifi_credentials.h"
text = "#pragma once\n#define ONLINE_WIFI_SSID " + json.dumps(ssid,ensure_ascii=False) + "\n"
text += "#define ONLINE_WIFI_PASSWORD " + json.dumps(password,ensure_ascii=False) + "\n"
text += "#define ONLINE_WIFI_CHANNEL " + str(wifi_channel) + "\n"
text += "#define ONLINE_DIAGNOSTIC_CHANNEL " + str(diagnostic_channel) + "\n"
text += peer_define
text += "#define ONLINE_NODE_A_MAC {" + ",".join(str(b) for b in node_a) + "}\n"
text += "#define ONLINE_NODE_B_MAC {" + ",".join(str(b) for b in node_b) + "}\n"
if not header.exists() or header.read_text(encoding="utf-8") != text:
    header.write_text(text,encoding="utf-8")
