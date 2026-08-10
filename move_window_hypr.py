#!/usr/bin/env python3
"""
Hyprland Random Window Mover – final version.

- Smoothly animates a random window using absolute pixel moves.
- Reliably preserves the tiled size when temporarily floating.
- Handles race conditions after toggling floating.
"""

import os
import socket
import json
import random
import time
import sys

# ─── Configuration ──────────────────────────────────────────────────────────

WORKSPACE_CHANGE_CHANCE = 0.08
SMOOTH_STEPS            = 25
SMOOTH_DURATION         = 0.5
EDGE_PADDING            = 40

# ─── IPC helpers (unchanged) ─────────────────────────────────────────────────

def get_socket_path():
    runtime_dir = os.environ.get('XDG_RUNTIME_DIR', '/tmp')
    instance_sig = os.environ.get('HYPRLAND_INSTANCE_SIGNATURE')
    if not instance_sig:
        hypr_dir = os.path.join(runtime_dir, 'hypr')
        if os.path.isdir(hypr_dir):
            candidates = [d for d in os.listdir(hypr_dir)
                         if os.path.isdir(os.path.join(hypr_dir, d))
                         and len(d) > 10 and d not in ('.', '..')]
            if candidates:
                instance_sig = candidates[0]
    if not instance_sig:
        print("Error: Hyprland instance signature not found.", file=sys.stderr)
        sys.exit(1)
    return os.path.join(runtime_dir, 'hypr', instance_sig, '.socket.sock')

def hyprctl(cmd):
    sock_path = get_socket_path()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    try:
        sock.connect(sock_path)
        sock.sendall(cmd.encode())
        sock.shutdown(socket.SHUT_WR)
        data = b''
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            data += chunk
        return data.decode()
    finally:
        sock.close()

def hyprctl_json(cmd):
    return json.loads(hyprctl(f'j/{cmd}'))

def get_window(address):
    for client in hyprctl_json('clients'):
        if client['address'] == address:
            return client
    return None

# ─── Robust floating + size preservation ────────────────────────────────────

def float_and_preserve_size(addr, tiled_w, tiled_h):
    """
    Float a tiled window and force its floating size to exactly (tiled_w, tiled_h).
    Retries until success or timeout.
    """
    print(f"Floating window {addr} and preserving tiled size ({tiled_w}x{tiled_h})...")

    # 1. Float it
    hyprctl(f'dispatch togglefloating address:{addr}')
    time.sleep(0.05)

    # 2. Wait until Hyprland actually makes it floating
    #    and then apply the exact size repeatedly until it sticks.
    max_attempts = 10
    for attempt in range(max_attempts):
        win = get_window(addr)
        if not win:
            print("  Window disappeared during floating.")
            return False

        if win.get('floating', False):
            curr_w, curr_h = win['size']
            # If size already matches, we're done
            if curr_w == tiled_w and curr_h == tiled_h:
                print("  Size preserved correctly.")
                return True

            # Otherwise force the correct size
            hyprctl(f'dispatch resizewindowpixel exact {tiled_w} {tiled_h},address:{addr}')
            time.sleep(0.05)
        else:
            # Still not floating – toggle again
            hyprctl(f'dispatch togglefloating address:{addr}')
            time.sleep(0.05)

    # If we exit the loop without success, try one last time then give up
    hyprctl(f'dispatch resizewindowpixel exact {tiled_w} {tiled_h},address:{addr}')
    time.sleep(0.1)
    win = get_window(addr)
    if win and win['size'] == [tiled_w, tiled_h]:
        return True
    print("  Warning: could not preserve exact tiled size after several attempts.")
    return True  # continue anyway

# ─── Smooth movement (absolute pixel positions) ─────────────────────────────

def smooth_move_to(addr, target_x, target_y, steps=SMOOTH_STEPS, duration=SMOOTH_DURATION):
    win = get_window(addr)
    if not win:
        print("Window not found for animation.")
        return

    start_x, start_y = win['at']
    step_delay = duration / steps

    for i in range(steps + 1):
        t = i / steps
        x = int(start_x + (target_x - start_x) * t)
        y = int(start_y + (target_y - start_y) * t)
        # Exact same command as the DVD-bounce bash script
        hyprctl(f'dispatch movewindowpixel exact {x} {y},address:{addr}')
        time.sleep(step_delay)

def pick_random_workspace_except(current_id):
    workspaces = hyprctl_json('workspaces')
    others = [w for w in workspaces
              if w['id'] != current_id and w['id'] > 0 and not w.get('special', False)]
    return random.choice(others) if others else None

# ─── Main ─────────────────────────────────────────────────────────────────

def main():
    active_ws = hyprctl_json('activeworkspace')
    ws_id = active_ws['id']

    candidates = [c for c in hyprctl_json('clients') if c['workspace']['id'] == ws_id]
    if not candidates:
        print("No windows on active workspace.")
        return

    window = random.choice(candidates)
    addr = window['address']
    title = window.get('title', 'Untitled')
    print(f"Selected: {title} ({addr})")

    active = hyprctl_json('activewindow')
    original_addr = active.get('address') if active else None

    if random.random() < WORKSPACE_CHANGE_CHANCE:
        target_ws = pick_random_workspace_except(ws_id)
        if target_ws:
            print(f"Teleporting to workspace '{target_ws['name']}'...")
            hyprctl(f'dispatch movetoworkspacesilent {target_ws["name"]},address:{addr}')
            if original_addr and original_addr != addr:
                time.sleep(0.05)
                hyprctl(f'dispatch focuswindow address:{original_addr}')
            return

    monitors = hyprctl_json('monitors')
    monitor = next((m for m in monitors if m.get('focused') or m['id'] == active_ws.get('monitorID')), monitors[0])

    was_tiled = not window.get('floating', False)

    if was_tiled:
        # Capture tiled size *before* any change
        orig_w, orig_h = window['size']
        if not float_and_preserve_size(addr, orig_w, orig_h):
            print("Failed to float window, aborting.")
            return
    else:
        print("Window is already floating.")

    # Get final floating state before animation
    window = get_window(addr)
    if not window:
        print("Window lost after floating.")
        return

    m_x, m_y = monitor['x'], monitor['y']
    m_w, m_h = monitor['width'], monitor['height']
    w_w, w_h = window['size']

    min_x = m_x + EDGE_PADDING
    max_x = m_x + m_w - w_w - EDGE_PADDING
    min_y = m_y + EDGE_PADDING
    max_y = m_y + m_h - w_h - EDGE_PADDING
    if max_x < min_x: max_x = min_x + 50
    if max_y < min_y: max_y = min_y + 50

    target_x = random.randint(int(min_x), int(max_x))
    target_y = random.randint(int(min_y), int(max_y))
    print(f"Moving smoothly to ({target_x}, {target_y}) on {monitor['name']}...")

    smooth_move_to(addr, target_x, target_y)

    # Re‑tile if it was originally tiled
    if was_tiled:
        print("Re‑tiling...")
        hyprctl(f'dispatch togglefloating address:{addr}')
        time.sleep(0.05)

    if original_addr and original_addr != addr:
        hyprctl(f'dispatch focuswindow address:{original_addr}')

    print("Done.")

if __name__ == '__main__':
    main()