# Pico GFX Pack (128x64 mono) demo: live HR + ECG sweep
#
# Display behavior:
# - HR value top-left
# - ECG drawn as a continuous ~2s window
# - left->right overwrite with a small clear gap ahead of the trace

import time
import struct
import polar_sdk

try:
    import machine
except ImportError:
    machine = None

try:
    import picographics as pg
    from picographics import PicoGraphics
except ImportError:
    print("This demo needs Pimoroni 'picographics' in firmware.")
    raise


DISPLAY_CANDIDATES = (
    "DISPLAY_GFX_PACK",
    "DISPLAY_PICO_DISPLAY_2",
    "DISPLAY_PICO_DISPLAY",
    "DISPLAY_PICO_EXPLORER",
    "DISPLAY_LCD_240X240",
    "DISPLAY_LCD_160X80",
    "DISPLAY_LCD_1IN14",
)


def make_display():
    for name in DISPLAY_CANDIDATES:
        if hasattr(pg, name):
            try:
                return PicoGraphics(display=getattr(pg, name)), name
            except Exception:
                pass
    raise RuntimeError("No supported PicoGraphics display constant found")


display, display_name = make_display()
WIDTH, HEIGHT = display.get_bounds()
if hasattr(display, "set_backlight"):
    display.set_backlight(1.0)

# Optional GFX Pack button + RGB backlight control (A..E buttons).
HAS_GFX_PACK_IO = display_name == "DISPLAY_GFX_PACK" and machine is not None
BTN_PINS = (12, 13, 14, 15, 22)  # A, B, C, D, E
RGB_PINS = (6, 7, 8)  # R, G, B
BTN_COLORS = (
    None,  # A -> pure white (white backlight channel)
    (0, 255, 0),  # B -> green
    (0, 0, 255),  # C -> blue
    (255, 120, 0),  # D -> amber
    (220, 0, 220),  # E -> magenta
)

buttons = None
pwms = None
button_prev = None
last_button_poll_ms = 0


# ---------- pens ----------
def pen_or_fallback(r, g, b, fallback):
    try:
        return display.create_pen(r, g, b)
    except Exception:
        return fallback


if display_name == "DISPLAY_GFX_PACK":
    BLACK = 0
    WHITE = 15
else:
    BLACK = pen_or_fallback(0, 0, 0, 0)
    WHITE = pen_or_fallback(255, 255, 255, 1)

# Visual style: dark background with bright waveform/text.
BG_PEN = BLACK
FG_PEN = WHITE


# ---------- layout ----------
TOP_H = 16
WAVE_TOP = TOP_H + 1
WAVE_BOTTOM = HEIGHT - 1
Y_MID = WAVE_TOP + (WAVE_BOTTOM - WAVE_TOP) // 2


# ---------- primitive drawing ----------
def hline(x0, x1, y, pen):
    if y < 0 or y >= HEIGHT:
        return
    if x0 > x1:
        x0, x1 = x1, x0
    if x1 < 0 or x0 >= WIDTH:
        return
    if x0 < 0:
        x0 = 0
    if x1 >= WIDTH:
        x1 = WIDTH - 1
    display.set_pen(pen)
    for x in range(x0, x1 + 1):
        display.pixel(x, y)


def vline(x, y0, y1, pen):
    if x < 0 or x >= WIDTH:
        return
    if y0 > y1:
        y0, y1 = y1, y0
    if y1 < 0 or y0 >= HEIGHT:
        return
    if y0 < 0:
        y0 = 0
    if y1 >= HEIGHT:
        y1 = HEIGHT - 1
    display.set_pen(pen)
    for y in range(y0, y1 + 1):
        display.pixel(x, y)


def fill_rect(x, y, w, h, pen):
    for yy in range(y, y + h):
        hline(x, x + w - 1, yy, pen)


def draw_line(x0, y0, x1, y1, pen):
    dx = x1 - x0
    sx = 1 if dx >= 0 else -1
    dx = dx if dx >= 0 else -dx

    dy = y1 - y0
    sy = 1 if dy >= 0 else -1
    dy = dy if dy >= 0 else -dy

    err = dx - dy
    display.set_pen(pen)

    while True:
        if 0 <= x0 < WIDTH and 0 <= y0 < HEIGHT:
            display.pixel(x0, y0)
        if x0 == x1 and y0 == y1:
            break
        e2 = err * 2
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy


def set_rgb_backlight(r, g, b):
    if not HAS_GFX_PACK_IO or pwms is None:
        return

    # Hide white channel so RGB color is visible.
    if hasattr(display, "set_backlight"):
        try:
            display.set_backlight(0.0)
        except Exception:
            pass

    vals = (r, g, b)
    for i in range(3):
        v = vals[i]
        if v < 0:
            v = 0
        if v > 255:
            v = 255
        pwms[i].duty_u16((v * 65535) // 255)


def set_white_backlight():
    if not HAS_GFX_PACK_IO or pwms is None:
        return

    # RGB off.
    for pwm in pwms:
        pwm.duty_u16(0)

    # White channel on.
    if hasattr(display, "set_backlight"):
        try:
            display.set_backlight(1.0)
        except Exception:
            pass


def init_buttons_backlight():
    global buttons, pwms, button_prev
    if not HAS_GFX_PACK_IO:
        return

    buttons = [machine.Pin(p, machine.Pin.IN, machine.Pin.PULL_UP) for p in BTN_PINS]
    button_prev = [1, 1, 1, 1, 1]

    pwms = []
    for p in RGB_PINS:
        pwm = machine.PWM(machine.Pin(p))
        pwm.freq(1000)
        pwm.duty_u16(0)
        pwms.append(pwm)

    set_white_backlight()


def poll_buttons(now):
    global last_button_poll_ms
    if not HAS_GFX_PACK_IO or buttons is None:
        return

    if time.ticks_diff(now, last_button_poll_ms) < 35:
        return
    last_button_poll_ms = now

    for i, btn in enumerate(buttons):
        v = btn.value()
        if button_prev[i] == 1 and v == 0:
            if BTN_COLORS[i] is None:
                set_white_backlight()
            else:
                set_rgb_backlight(*BTN_COLORS[i])
        button_prev[i] = v


# ---------- header digits ----------
DIGITS = {
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "010", "010", "010"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "-": ("000", "000", "111", "000", "000"),
}

TINY = {
    "R": ("110", "101", "110", "101", "101"),
    "b": ("100", "100", "110", "101", "110"),
    "p": ("000", "110", "101", "110", "100"),
    "m": ("000", "110", "101", "101", "101"),
    "s": ("011", "100", "010", "001", "110"),
    ":": ("000", "010", "000", "010", "000"),
    " ": ("000", "000", "000", "000", "000"),
}

S = 2
DW = 3 * S
DG = S


def draw_digit(ch, x, y):
    pat = DIGITS.get(ch, DIGITS["-"])
    for ry, row in enumerate(pat):
        for rx, bit in enumerate(row):
            if bit == "1":
                fill_rect(x + rx * S, y + ry * S, S, S, FG_PEN)


def draw_tiny_char(ch, x, y):
    if ch in DIGITS:
        pat = DIGITS[ch]
    else:
        pat = TINY.get(ch, TINY[" "])
    for ry, row in enumerate(pat):
        for rx, bit in enumerate(row):
            if bit == "1":
                display.set_pen(FG_PEN)
                display.pixel(x + rx, y + ry)


def draw_tiny_text(text, x, y):
    for ch in text:
        draw_tiny_char(ch, x, y)
        x += 4  # 3px glyph + 1px gap


def draw_header(hr, rr):
    fill_rect(0, 0, WIDTH, TOP_H, BG_PEN)

    # Top-left aligned HR.
    hr_num = str(hr) if hr > 0 else "--"
    hr_line = hr_num + " bpm"
    draw_tiny_text(hr_line, 1, 1)

    # Top-right aligned RR.
    rr_num = str(rr) if rr > 0 else "----"
    rr_line = "R-R: " + rr_num + " ms"
    rr_x = WIDTH - (len(rr_line) * 4) - 1
    draw_tiny_text(rr_line, rr_x, 1)


# ---------- ECG sweep ----------
x = 0
prev_y = Y_MID
CHASE_GAP = 12
trace_y = [Y_MID] * WIDTH
tail_phase = 0
head_extra_x = -1
head_extra_y = -1


def clear_wave_area():
    global head_extra_x, head_extra_y
    head_extra_x = -1
    head_extra_y = -1
    for i in range(WIDTH):
        trace_y[i] = Y_MID
    fill_rect(0, WAVE_TOP, WIDTH, HEIGHT - WAVE_TOP, BG_PEN)


def sweep_to(y):
    global x, prev_y, tail_phase, head_extra_x, head_extra_y
    nx = x + 1
    wrapped = False
    if nx >= WIDTH:
        nx = 0
        wrapped = True

    # Remove previous head "extra" pixel so only the current tip is 2px.
    if 0 <= head_extra_x < WIDTH and WAVE_TOP <= head_extra_y <= WAVE_BOTTOM:
        display.set_pen(BG_PEN)
        display.pixel(head_extra_x, head_extra_y)
    head_extra_x = -1
    head_extra_y = -1

    # Clear region ahead (gap before incoming head).
    for i in range(CHASE_GAP):
        cx = nx + i
        if cx >= WIDTH:
            cx -= WIDTH
        vline(cx, WAVE_TOP, WAVE_BOTTOM, BG_PEN)

    # Tail effect: keep only dotted samples at the very outgoing edge.
    tx = nx + CHASE_GAP
    if tx >= WIDTH:
        tx -= WIDTH
    vline(tx, WAVE_TOP, WAVE_BOTTOM, BG_PEN)
    tail_y = trace_y[tx]
    if tail_phase == 0 and WAVE_TOP <= tail_y <= WAVE_BOTTOM:
        display.set_pen(FG_PEN)
        display.pixel(tx, tail_y)
    tail_phase ^= 1

    # Don't draw a line across the wrap boundary (right edge -> left edge).
    if wrapped:
        x = nx
        prev_y = y
        trace_y[nx] = y
    else:
        # Main wave is 1px.
        draw_line(x, prev_y, nx, y, FG_PEN)
        x = nx
        prev_y = y
        trace_y[nx] = y

    # Tip marker: exactly one extra pixel (2px total at the head).
    ey = y - 1
    if ey < WAVE_TOP:
        ey = y + 1
    if WAVE_TOP <= ey <= WAVE_BOTTOM:
        display.set_pen(FG_PEN)
        display.pixel(nx, ey)
        head_extra_x = nx
        head_extra_y = ey


# ---------- y queue (decouples chunked BLE reads from smooth drawing cadence) ----------
YQ_CAP = 512
yq = [Y_MID] * YQ_CAP
yq_head = 0
yq_tail = 0
yq_count = 0


def yq_clear():
    global yq_head, yq_tail, yq_count
    yq_head = 0
    yq_tail = 0
    yq_count = 0


def yq_push(v):
    global yq_tail, yq_head, yq_count
    yq[yq_tail] = v
    yq_tail += 1
    if yq_tail >= YQ_CAP:
        yq_tail = 0
    if yq_count < YQ_CAP:
        yq_count += 1
    else:
        # overwrite oldest
        yq_head += 1
        if yq_head >= YQ_CAP:
            yq_head = 0


def yq_pop():
    global yq_head, yq_count
    if yq_count <= 0:
        return None
    v = yq[yq_head]
    yq_head += 1
    if yq_head >= YQ_CAP:
        yq_head = 0
    yq_count -= 1
    return v


# ---------- Polar ----------
h10 = polar_sdk.Device(
    name_prefix="Polar",
    required_capabilities=(
        polar_sdk.CAP_STREAM_HR,
        polar_sdk.CAP_STREAM_ECG,
    ),
)

last_hr = 0
last_rr = 0
hr_running = False
ecg_running = False
last_hr_try_ms = 0
last_ecg_try_ms = 0
last_ecg_data_ms = 0
ecg_seen_data = False

# Sample processing tuning
# 130Hz ECG, 2 samples/pixel ~= 65px/s => ~2s window on 128px width.
SAMPLES_PER_PIXEL = 2
sample_acc = 0
sample_acc_n = 0
dc = 0
smooth = 0
peak_abs = 1400

# Draw cadence for smooth sweep.
DRAW_PERIOD_MS = 15
next_draw_ms = 0
last_header_ms = 0
last_update_ms = 0


def try_start_hr(now):
    global hr_running, last_hr_try_ms
    if hr_running or time.ticks_diff(now, last_hr_try_ms) < 2000:
        return
    last_hr_try_ms = now
    try:
        h10.start_hr()
        hr_running = True
        print("HR started")
    except Exception:
        pass


def try_start_ecg(now):
    global ecg_running, last_ecg_try_ms, last_ecg_data_ms, ecg_seen_data
    if ecg_running or time.ticks_diff(now, last_ecg_try_ms) < 2000:
        return
    last_ecg_try_ms = now
    try:
        h10.start_ecg(sample_rate=130)
        ecg_running = True
        ecg_seen_data = False
        last_ecg_data_ms = now
        yq_clear()
        print("ECG started")
    except Exception:
        pass


print("polar_sdk", polar_sdk.version())
print("display", display_name, WIDTH, "x", HEIGHT)

try:
    draw_header(0, 0)
    clear_wave_area()
    display.update()

    init_buttons_backlight()

    h10.connect(timeout_ms=15000)
    now = time.ticks_ms()
    next_draw_ms = now

    # ECG first is usually more reliable.
    try_start_ecg(now)
    try_start_hr(now)

    while True:
        now = time.ticks_ms()
        poll_buttons(now)
        try_start_ecg(now)
        try_start_hr(now)

        # HR polling
        if hr_running:
            try:
                hr = h10.read_hr(timeout_ms=0)
                if hr is not None:
                    last_hr = hr[1]
                    rr_count = hr[2]
                    if rr_count > 0:
                        last_rr = hr[3]
            except Exception:
                hr_running = False

        # ECG ingest -> y queue
        if ecg_running:
            try:
                chunk = h10.read_ecg(max_bytes=1024, timeout_ms=0)
            except Exception:
                ecg_running = False
                chunk = None

            if chunk:
                last_ecg_data_ms = now
                ecg_seen_data = True
                mv = memoryview(chunk)
                n = len(mv) // 4

                for i in range(n):
                    s = struct.unpack_from("<i", mv, i * 4)[0]
                    sample_acc += s
                    sample_acc_n += 1
                    if sample_acc_n < SAMPLES_PER_PIXEL:
                        continue

                    s = sample_acc // sample_acc_n
                    sample_acc = 0
                    sample_acc_n = 0

                    # Baseline removal + smoothing
                    dc = (dc * 63 + s) // 64
                    s -= dc
                    smooth = (smooth * 3 + s) // 4

                    a = smooth if smooth >= 0 else -smooth
                    # Slow AGC to reduce jumping
                    peak_abs = (peak_abs * 255) // 256
                    if a > peak_abs:
                        peak_abs = a
                    if peak_abs < 300:
                        peak_abs = 300

                    # Boost amplitude (~2x) so ECG is less tiny on 64px height.
                    display_peak = peak_abs // 2
                    if display_peak < 180:
                        display_peak = 180

                    amp = (WAVE_BOTTOM - WAVE_TOP) // 2 - 2
                    y = Y_MID - (smooth * amp) // display_peak
                    if y < WAVE_TOP:
                        y = WAVE_TOP
                    if y > WAVE_BOTTOM:
                        y = WAVE_BOTTOM

                    yq_push(y)

            # stale ECG -> restart
            if ecg_seen_data and time.ticks_diff(now, last_ecg_data_ms) > 4000:
                try:
                    h10.stop_ecg()
                except Exception:
                    pass
                ecg_running = False
                ecg_seen_data = False

        # Steady playback cadence from queue (smooth sweep)
        now = time.ticks_ms()
        while time.ticks_diff(now, next_draw_ms) >= 0:
            y = yq_pop()
            if y is None:
                # keep continuity if queue temporarily empty
                y = prev_y
            sweep_to(y)
            next_draw_ms = time.ticks_add(next_draw_ms, DRAW_PERIOD_MS)

        if time.ticks_diff(now, last_header_ms) > 150:
            draw_header(last_hr, last_rr)
            last_header_ms = now

        if time.ticks_diff(now, last_update_ms) > 55:
            display.update()
            last_update_ms = now

        time.sleep_ms(6)

except KeyboardInterrupt:
    print("stopped")
finally:
    try:
        h10.stop_ecg()
    except Exception:
        pass
    try:
        h10.stop_hr()
    except Exception:
        pass
    try:
        h10.disconnect()
    except Exception:
        pass

    if pwms is not None:
        for pwm in pwms:
            try:
                pwm.deinit()
            except Exception:
                pass
