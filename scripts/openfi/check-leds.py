#!/usr/bin/env python3
"""
核对：board.d/01_leds 里给 openfi,6c 引用的 LED sysfs 名，
      和 DTS 里实际定义的灯，能不能对得上。

为什么需要这个检查：
  上游给 openfi,6c 写的那段是从别的板子抄的（引用了 green:lan 和 eth0，
  两者在本板上都不存在）。灯名写错时内核【不会报错】—— 只是灯不亮，
  上机才发现。所以放进 CI，让它在编译前就拦住。
"""
import re
import sys

LEDS = 'target/linux/mediatek/filogic/base-files/etc/board.d/01_leds'
DTS = 'target/linux/mediatek/dts/mt7981b-openfi-6c.dts'

CMAP = {
    'LED_COLOR_ID_GREEN': 'green', 'LED_COLOR_ID_BLUE': 'blue',
    'LED_COLOR_ID_RED': 'red', 'LED_COLOR_ID_AMBER': 'amber',
    'LED_COLOR_ID_WHITE': 'white', 'LED_COLOR_ID_YELLOW': 'yellow',
    'LED_COLOR_ID_PURPLE': 'purple', 'LED_COLOR_ID_ORANGE': 'orange',
}
FMAP = {
    'LED_FUNCTION_STATUS': 'status', 'LED_FUNCTION_WAN': 'wan',
    'LED_FUNCTION_WLAN': 'wlan', 'LED_FUNCTION_MOBILE': 'mobile',
    'LED_FUNCTION_LAN': 'lan', 'LED_FUNCTION_POWER': 'power',
    'LED_FUNCTION_WLAN_2GHZ': 'wlan-2ghz', 'LED_FUNCTION_WLAN_5GHZ': 'wlan-5ghz',
    'LED_FUNCTION_WAN_2': 'wan2', 'LED_FUNCTION_USB': 'usb',
}

def leds_from_boardd(path):
    s = open(path, encoding='utf-8').read()
    m = re.search(r'openfi,6c\)(.*?);;', s, re.S)
    if not m:
        return None
    # 去掉整行注释，免得注释里的例子被当成真实引用
    body = "\n".join(l for l in m.group(1).splitlines()
                     if not l.strip().startswith('#'))
    return sorted(set(re.findall(r'"([a-z]+:[a-z0-9-]+)"', body)))

def leds_from_dts(path):
    s = open(path, encoding='utf-8').read()
    # 定位 leds { ... } 段
    m = re.search(r'\n\s*leds\s*\{', s)
    if not m:
        return []
    i = s.index('{', m.start())
    depth, j = 0, i
    while j < len(s):
        if s[j] == '{':
            depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = s[i:j]
    out = []
    # 逐个 led-<n> { ... } 块
    for bm in re.finditer(r'led-\d+\s*\{', body):
        k = body.index('{', bm.start())
        d, e = 0, k
        while e < len(body):
            if body[e] == '{':
                d += 1
            elif body[e] == '}':
                d -= 1
                if d == 0:
                    break
            e += 1
        blk = body[k:e]
        c = re.search(r'color\s*=\s*<(LED_COLOR_ID_\w+)>', blk)
        f = re.search(r'function\s*=\s*<?(LED_FUNCTION_\w+)>?', blk)
        if not (c and f):
            continue
        col = CMAP.get(c.group(1), c.group(1).lower())
        fun = FMAP.get(f.group(1), f.group(1).lower())
        out.append('%s:%s' % (col, fun))
    return sorted(set(out))

used = leds_from_boardd(LEDS)
have = leds_from_dts(DTS)
if used is None:
    print("01_leds 里没有 openfi,6c 这一段")
    sys.exit(1)

print("  01_leds 引用 :", ", ".join(used) if used else "(无)")
print("  DTS 实际提供 :", ", ".join(have) if have else "(无)")

bad = [x for x in used if x not in have]
if bad:
    print("  ❌ 这些灯 01_leds 引用了但 DTS 里没有(灯不会亮):", ", ".join(bad))
    sys.exit(1)
miss = [x for x in have if x not in used]
if miss:
    print("  ℹ️  DTS 有但没配 trigger 的灯(留给用户态控制,如 green:mobile):",
          ", ".join(miss))
print("  ✅ 灯名全部对得上")
