#!/usr/bin/env python3
"""
Ollama stub server with a fixed sidebar UI.

Goals:
- Provide a friendly terminal UI with a stable command sidebar.
- Log every incoming request and show what response is sent.
- Let the user queue responses (actions + planner goals) from the console.
- Be resilient to bad input and server errors.

Implements a minimal subset of Ollama's /api/generate endpoint used by the module.
"""
from __future__ import annotations

import argparse
import errno
import json
import shutil
import sys
import textwrap
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Queue, Empty
from typing import Any, Dict, List, Optional

DISTANCE_BANDS = ["very close", "close", "medium", "medium far", "far"]


def now_iso() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())


def now_short() -> str:
    return time.strftime("%H:%M:%S", time.localtime())


class ConsoleUI:
    def __init__(self, max_logs: int = 1000) -> None:
        self._lock = threading.Lock()
        self._logs: deque[str] = deque(maxlen=max_logs)
        self._plain = not sys.stdout.isatty()
        self._warned_small = False
        self._host = "127.0.0.1"
        self._port = 11435

    @property
    def plain(self) -> bool:
        return self._plain

    def set_server(self, host: str, port: int) -> None:
        self._host = host
        self._port = port

    def log(self, message: str) -> None:
        ts = now_short()
        prefix = f"[{ts}] "
        lines = message.splitlines() or [""]
        entries = []
        for i, line in enumerate(lines):
            if i == 0:
                entries.append(prefix + line)
            else:
                entries.append(" " * len(prefix) + line)

        with self._lock:
            for entry in entries:
                self._logs.append(entry)

        if self._plain:
            for entry in entries:
                print(entry, flush=True)

    def render(self, state: "StubState") -> None:
        if self._plain:
            return

        size = shutil.get_terminal_size(fallback=(120, 40))
        width, height = size.columns, size.lines
        layout = self._layout(width, height)
        if layout is None:
            if not self._warned_small:
                self._warned_small = True
                self._plain = True
                self.log("terminal too small for sidebar UI; falling back to plain output.")
            return

        left_width, right_width, rows = layout

        with self._lock:
            log_lines = list(self._logs)

        left_lines: List[str] = []
        for line in log_lines:
            left_lines.extend(self._wrap_line(line, left_width))

        if len(left_lines) < rows:
            left_lines = [""] * (rows - len(left_lines)) + left_lines
        else:
            left_lines = left_lines[-rows:]

        sidebar_lines = self._build_sidebar(state, right_width, rows)

        sys.stdout.write("\x1b[2J\x1b[H")
        for idx in range(rows):
            left = left_lines[idx][:left_width].ljust(left_width)
            right = ""
            if idx < len(sidebar_lines):
                right = sidebar_lines[idx][:right_width].ljust(right_width)
            else:
                right = " " * right_width
            sys.stdout.write(left + " " + right + "\n")
        sys.stdout.flush()

    def _layout(self, width: int, height: int) -> Optional[tuple[int, int, int]]:
        if width < 60 or height < 8:
            return None
        sidebar = 42 if width >= 100 else max(28, width // 3)
        left = width - sidebar - 1
        if left < 20:
            return None
        rows = max(1, height - 1)
        return left, sidebar, rows

    def _wrap_line(self, line: str, width: int) -> List[str]:
        if width <= 0:
            return [""]
        if len(line) <= width:
            return [line]
        indent = len(line) - len(line.lstrip(" "))
        return textwrap.wrap(
            line,
            width=width,
            subsequent_indent=" " * min(indent, max(0, width - 1)),
            drop_whitespace=False,
            replace_whitespace=False,
        )

    def _build_sidebar(self, state: "StubState", width: int, rows: int) -> List[str]:
        with state.lock:
            try:
                aq = state.action_queue.qsize()
                lq = state.long_term_goal_queue.qsize()
                sq = state.short_term_goals_queue.qsize()
            except Exception:
                aq = lq = sq = 0
            last_role = state.last_role
            last_model = state.last_request_model or "-"
            last_prompt_len = state.last_request_prompt_len
            last_at = state.last_request_at

        last_at_text = "-"
        if last_at:
            last_at_text = time.strftime("%H:%M:%S", time.localtime(last_at))

        lines = [
            "Ollama Stub (bot-amigo)",
            f"URL: http://{self._host}:{self._port}/api/generate",
            "",
            "Queues",
            f" actions: {aq}",
            f" long:    {lq}",
            f" short:   {sq}",
            "",
            "Last Request",
            f" role:   {last_role}",
            f" model:  {last_model}",
            f" prompt: {last_prompt_len} chars",
            f" time:   {last_at_text}",
            "",
            "Commands",
            " status | history [n]",
            " last   | help | quit",
            "",
            "Control",
            " action <name> [json]",
            " idle | move <idx|dir>",
            " grind | stay | unstay",
            " talk [quest_id]",
            " action request_profession",
            "   {\"skill\":\"fishing\",",
            "    \"intent\":\"fish\"}",
            "",
            "Planner",
            " long <text>",
            " short (multi-line)",
            "",
            "Notes",
            " attack is legacy",
            " band is legacy",
        ]

        trimmed = [self._trim_line(line, width) for line in lines]
        if len(trimmed) < rows:
            trimmed += [""] * (rows - len(trimmed))
        return trimmed[:rows]

    def _trim_line(self, line: str, width: int) -> str:
        if len(line) <= width:
            return line
        if width <= 1:
            return line[:width]
        return line[: max(0, width - 3)] + "..."


@dataclass
class RequestRecord:
    ts: float
    role: str
    model: str
    prompt: str


@dataclass
class StubState:
    lock: threading.Lock = field(default_factory=threading.Lock)
    action_queue: Queue[Dict[str, Any]] = field(default_factory=Queue)
    long_term_goal_queue: Queue[str] = field(default_factory=Queue)
    short_term_goals_queue: Queue[List[str]] = field(default_factory=Queue)

    distance_band_index: int = 1
    nav_target_index: int = 0
    nav_epoch: int = 1
    quest_id: int = 1

    history: List[RequestRecord] = field(default_factory=list)
    max_history: int = 50

    last_action_sent: Optional[Dict[str, Any]] = None
    last_long_term_goal_sent: Optional[str] = None
    last_short_term_goals_sent: Optional[List[str]] = None
    last_response_text: str = ""
    last_role: str = "unknown"

    last_request_at: Optional[float] = None
    last_request_model: str = ""
    last_request_prompt_len: int = 0

    def add_history(self, rec: RequestRecord) -> None:
        with self.lock:
            self.history.append(rec)
            if len(self.history) > self.max_history:
                self.history = self.history[-self.max_history :]

    def current_distance_band(self) -> str:
        with self.lock:
            return DISTANCE_BANDS[self.distance_band_index]

    def set_distance_band(self, idx: int) -> None:
        with self.lock:
            self.distance_band_index = max(0, min(idx, len(DISTANCE_BANDS) - 1))

    def set_nav_target_index(self, idx: int) -> None:
        with self.lock:
            self.nav_target_index = max(0, idx)

    def set_nav_epoch(self, epoch: int) -> None:
        with self.lock:
            self.nav_epoch = max(0, epoch)

    def set_quest_id(self, qid: int) -> None:
        with self.lock:
            self.quest_id = max(0, qid)

    def enqueue_action(self, name: str, arguments: Dict[str, Any]) -> None:
        self.action_queue.put({"name": name, "arguments": arguments})

    def enqueue_long_term_goal(self, goal: str) -> None:
        self.long_term_goal_queue.put(goal)

    def enqueue_short_term_goals(self, goals: List[str]) -> None:
        self.short_term_goals_queue.put(goals)

    def consume_action(self) -> Dict[str, Any]:
        try:
            action = self.action_queue.get_nowait()
        except Empty:
            action = {"name": "request_idle", "arguments": {}}
        if not isinstance(action, dict):
            action = {"name": "request_idle", "arguments": {}}
        if not isinstance(action.get("name"), str) or not action.get("name"):
            action = {"name": "request_idle", "arguments": {}}
        if not isinstance(action.get("arguments"), dict):
            action["arguments"] = {}
        with self.lock:
            self.last_action_sent = action
        return action

    def consume_long_term_goal(self) -> str:
        try:
            goal = self.long_term_goal_queue.get_nowait()
        except Empty:
            goal = "Complete the most relevant nearby objective safely."
        with self.lock:
            self.last_long_term_goal_sent = goal
        return goal

    def consume_short_term_goals(self) -> List[str]:
        defaults = [
            "Scan nearby quest givers or objectives and pick the most relevant next step.",
            "Move carefully toward the closest relevant objective or NPC; avoid unnecessary combat.",
            "Execute the task and reassess; if progress stalls, reposition and try an alternate approach.",
        ]
        try:
            goals = self.short_term_goals_queue.get_nowait()
        except Empty:
            goals = defaults
        with self.lock:
            self.last_short_term_goals_sent = goals
        return goals

    def consume_planner_response(self, prompt: str) -> str:
        token = prompt.lower()
        if "short-term goals" in token or "short_term_goal" in token:
            goals = self.consume_short_term_goals()
            return "\n".join(goals)
        if "proposed_long_term_goal" in token or "proposed long-term goal" in token:
            with self.lock:
                if self.last_long_term_goal_sent:
                    return self.last_long_term_goal_sent
            return self.consume_long_term_goal()
        return self.consume_long_term_goal()


def classify_role(prompt: str, model: str) -> str:
    token = f"{model} {prompt}".lower()
    if "tool_call" in token or "<tool_call>" in token:
        return "action"
    if "long-term goal" in token or "short-term goals" in token or "long_term_goal" in token:
        return "planner"
    return "unknown"


def format_tool_call(name: str, arguments: Dict[str, Any]) -> str:
    return "<tool_call>\n" + json.dumps({"name": name, "arguments": arguments}) + "\n</tool_call>"


class StubHandler(BaseHTTPRequestHandler):
    state: StubState
    ui: ConsoleUI

    def do_POST(self) -> None:
        try:
            if self.path != "/api/generate":
                self.send_response(404)
                self.end_headers()
                return

            length = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(length).decode("utf-8") if length else "{}"
            try:
                payload = json.loads(raw)
                if not isinstance(payload, dict):
                    payload = {}
            except Exception as exc:
                self.ui.log(f"invalid JSON payload; using empty payload. error={exc}")
                payload = {}

            prompt = payload.get("prompt", "") or ""
            model = payload.get("model", "") or ""
            if not isinstance(prompt, str):
                prompt = str(prompt)
            if not isinstance(model, str):
                model = str(model)

            role = classify_role(prompt, model)

            with self.state.lock:
                self.state.last_role = role
                self.state.last_request_at = time.time()
                self.state.last_request_model = model
                self.state.last_request_prompt_len = len(prompt)

            self.state.add_history(RequestRecord(ts=time.time(), role=role, model=model, prompt=prompt))

            try:
                aq = self.state.action_queue.qsize()
                lq = self.state.long_term_goal_queue.qsize()
                sq = self.state.short_term_goals_queue.qsize()
            except Exception:
                aq = lq = sq = 0

            self.ui.log(
                "incoming request "
                f"(role={role}, model={model or '-'}, prompt_len={len(prompt)}, "
                f"queues: actions={aq}, long={lq}, short={sq})"
            )

            if role == "action":
                tool_call = self.state.consume_action()
                response_text = format_tool_call(tool_call["name"], tool_call.get("arguments", {}))
                self.ui.log(f"responding with action: {tool_call['name']}")
            elif role == "planner":
                response_text = self.state.consume_planner_response(prompt)
                preview = response_text.replace("\n", " ")
                if len(preview) > 60:
                    preview = preview[:57] + "..."
                self.ui.log(f"responding with planner text: {preview}")
            else:
                response_text = "OK"
                self.ui.log("responding with default text")

            with self.state.lock:
                self.state.last_response_text = response_text

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write((json.dumps({"response": response_text}) + "\n").encode("utf-8"))
        except Exception as exc:
            self.ui.log(f"error handling request: {exc}")
            try:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": "internal error"}).encode("utf-8"))
            except Exception:
                return

    def log_message(self, *_: Any) -> None:
        return


def run_server(state: StubState, ui: ConsoleUI, host: str, port: int) -> ThreadingHTTPServer:
    handler = type("InjectedHandler", (StubHandler,), {"state": state, "ui": ui})
    httpd = ThreadingHTTPServer((host, port), handler)
    thread = threading.Thread(target=httpd.serve_forever, name="ollama-stub-server", daemon=True)
    thread.start()
    return httpd


def _parse_json_maybe(text: str) -> Dict[str, Any]:
    text = text.strip()
    if not text:
        return {}
    try:
        val = json.loads(text)
        if isinstance(val, dict):
            return val
        raise ValueError("JSON must be an object")
    except Exception as exc:
        raise ValueError(f"invalid JSON: {exc}") from exc


def show_status(state: StubState) -> List[str]:
    with state.lock:
        lines = [
            "STATUS",
            f"time:               {now_iso()}",
            f"queued actions:     {state.action_queue.qsize()}",
            f"queued long goals:  {state.long_term_goal_queue.qsize()}",
            f"queued short goals: {state.short_term_goals_queue.qsize()}",
            f"distance_band:      {state.current_distance_band()}",
            f"nav_target_index:   {state.nav_target_index}",
            f"nav_epoch:          {state.nav_epoch}",
            f"quest_id:           {state.quest_id}",
            f"last_role:          {state.last_role}",
        ]
        if state.last_action_sent:
            lines.append(f"last_action_sent:   {json.dumps(state.last_action_sent)}")
        if state.last_long_term_goal_sent:
            lines.append(f"last_long_term_goal:{state.last_long_term_goal_sent}")
        if state.last_short_term_goals_sent:
            lines.append(f"last_short_term_goals:{json.dumps(state.last_short_term_goals_sent)}")
        if state.last_response_text:
            lines.append(f"last_response_len:  {len(state.last_response_text)}")
        return lines


def show_history(state: StubState, n: int = 10) -> List[str]:
    with state.lock:
        items = state.history[-n:]
    if not items:
        return ["No history yet."]
    lines = ["RECENT REQUESTS"]
    for i, rec in enumerate(items, 1):
        ts = time.strftime("%H:%M:%S", time.localtime(rec.ts))
        preview = rec.prompt.replace("\n", " ")[:120]
        lines.append(f"{i:02d}. [{ts}] role={rec.role} model={rec.model} prompt='{preview}'")
    return lines


def help_text() -> str:
    return """
Commands:
  status
  history [n]
  last
  help
  quit

Control:
  action <name> [json_args]
    example: action request_idle {}
    example: action request_move_hop {"nav_epoch":42,"candidate_id":"nav_0"}
  idle | move <idx|candidate_id|direction>
  grind | stay | unstay | talk [quest_id]
  action request_profession {"skill":"fishing","intent":"fish"}

Planner:
  long <text>
  short  (enter 1+ lines; end with a single '.' line)

Knobs (legacy):
  band <idx|label>
  nav <idx>
  epoch <n>
  quest <id>
""".strip()


def console_loop(state: StubState, ui: ConsoleUI) -> None:
    if ui.plain:
        print(help_text(), flush=True)
    else:
        ui.log("commands are listed in the sidebar. type 'help' for a plain list.")

    while True:
        if not ui.plain:
            ui.render(state)
        try:
            cmdline = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            ui.log("Exiting.")
            break

        if not cmdline:
            continue

        parts = cmdline.split(maxsplit=2)
        cmd = parts[0].lower()

        try:
            if cmd in ("quit", "exit", "q"):
                break

            elif cmd in ("help", "h", "?"):
                if ui.plain:
                    print(help_text(), flush=True)
                else:
                    ui.log("commands are listed in the sidebar. type 'help' in plain mode for full list.")

            elif cmd == "status":
                ui.log("\n".join(show_status(state)))

            elif cmd == "history":
                n = 10
                if len(parts) >= 2:
                    try:
                        n = int(parts[1])
                    except ValueError:
                        ui.log("history expects an integer count")
                        continue
                ui.log("\n".join(show_history(state, n=n)))

            elif cmd == "last":
                with state.lock:
                    last = state.last_response_text or "(none)"
                ui.log("LAST RESPONSE\n" + last)

            elif cmd == "action":
                if len(parts) < 2:
                    ui.log("usage: action <name> [json_args]")
                    continue
                name = parts[1]
                args = {}
                if len(parts) == 3:
                    args = _parse_json_maybe(parts[2])
                state.enqueue_action(name, args)
                ui.log(f"queued action: {name} {json.dumps(args)}")

            elif cmd == "idle":
                state.enqueue_action("request_idle", {})
                ui.log("queued action: request_idle")

            elif cmd == "grind":
                state.enqueue_action("request_enter_grind", {})
                ui.log("queued action: request_enter_grind")

            elif cmd in ("stopgrind", "ungrind", "follow"):
                state.enqueue_action("request_stop_grind", {})
                ui.log("queued action: request_stop_grind")

            elif cmd == "attack":
                state.enqueue_action("request_enter_attack_pull", {})
                ui.log("queued action: request_enter_attack_pull (legacy)")

            elif cmd == "stay":
                state.enqueue_action("request_stay", {})
                ui.log("queued action: request_stay")

            elif cmd == "unstay":
                state.enqueue_action("request_unstay", {})
                ui.log("queued action: request_unstay")

            elif cmd == "talk":
                if len(parts) >= 2:
                    try:
                        qid = int(parts[1])
                    except ValueError:
                        ui.log("talk expects a numeric quest id")
                        continue
                else:
                    with state.lock:
                        qid = state.quest_id
                state.enqueue_action("request_talk_to_quest_giver", {"quest_id": qid})
                ui.log(f"queued action: request_talk_to_quest_giver {{\"quest_id\": {qid}}}")

            elif cmd == "move":
                if len(parts) < 2:
                    ui.log("usage: move <idx|candidate_id|forward|backward|left|right>")
                    continue

                token = parts[1].strip().lower()
                if token.startswith("nav_"):
                    candidate_id = token
                elif token.isdigit():
                    candidate_id = f"nav_{int(token)}"
                else:
                    mapping = {"forward": "nav_0", "backward": "nav_1", "left": "nav_2", "right": "nav_3"}
                    candidate_id = mapping.get(token, "")
                    if not candidate_id:
                        ui.log("unknown direction. use forward/backward/left/right or nav_<idx>.")
                        continue

                with state.lock:
                    epoch = state.nav_epoch

                state.enqueue_action("request_move_hop", {"nav_epoch": epoch, "candidate_id": candidate_id})
                ui.log(f"queued action: request_move_hop {{\"nav_epoch\":{epoch},\"candidate_id\":\"{candidate_id}\"}}")

            elif cmd == "long":
                if len(parts) < 2:
                    ui.log("usage: long <text>")
                    continue
                text = cmdline[len("long ") :].strip()
                state.enqueue_long_term_goal(text)
                ui.log("queued long-term goal.")

            elif cmd == "short":
                if ui.plain:
                    print("Enter short-term goals, one per line. End with a single '.' line.", flush=True)
                else:
                    ui.log("enter short-term goals; end with a single '.' line.")
                goals: List[str] = []
                while True:
                    line = input(".. ").rstrip()
                    if line.strip() == ".":
                        break
                    if line.strip():
                        goals.append(line.strip())
                if not goals:
                    ui.log("no goals entered; nothing queued.")
                    continue
                state.enqueue_short_term_goals(goals)
                ui.log(f"queued {len(goals)} short-term goals.")

            elif cmd == "band":
                if len(parts) < 2:
                    ui.log("usage: band <idx|label>")
                    continue
                val = parts[1].lower()
                if val.isdigit():
                    state.set_distance_band(int(val))
                else:
                    labels = [b.lower() for b in DISTANCE_BANDS]
                    if val not in labels:
                        ui.log(f"unknown band label. valid: {DISTANCE_BANDS}")
                        continue
                    state.set_distance_band(labels.index(val))
                ui.log(f"distance_band now: {state.current_distance_band()}")

            elif cmd == "nav":
                if len(parts) < 2:
                    ui.log("usage: nav <idx>")
                    continue
                try:
                    nav_idx = int(parts[1])
                except ValueError:
                    ui.log("nav expects an integer index")
                    continue
                state.set_nav_target_index(nav_idx)
                with state.lock:
                    ui.log(f"nav_target_index now: {state.nav_target_index}")

            elif cmd == "epoch":
                if len(parts) < 2:
                    ui.log("usage: epoch <n>")
                    continue
                try:
                    epoch = int(parts[1])
                except ValueError:
                    ui.log("epoch must be an integer")
                    continue
                state.set_nav_epoch(epoch)
                with state.lock:
                    ui.log(f"nav_epoch now: {state.nav_epoch}")

            elif cmd == "quest":
                if len(parts) < 2:
                    ui.log("usage: quest <id>")
                    continue
                try:
                    quest_id = int(parts[1])
                except ValueError:
                    ui.log("quest expects a numeric id")
                    continue
                state.set_quest_id(quest_id)
                with state.lock:
                    ui.log(f"quest_id now: {state.quest_id}")

            else:
                ui.log(f"unknown command: {cmd}. type 'help'.")

        except Exception as exc:
            ui.log(f"error: {exc}")


def prompt_for_port(default_port: int) -> int:
    while True:
        raw = input(f"Port to bind [default {default_port}]: ").strip()
        if not raw:
            return default_port
        if raw.isdigit():
            port = int(raw)
            if 1 <= port <= 65535:
                return port
        print("Please enter a valid port between 1 and 65535.", flush=True)


def main() -> None:
    ap = argparse.ArgumentParser(description="Ollama stub server with a sidebar UI.")
    ap.add_argument("--host", default="127.0.0.1", help="bind host (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=11435, help="bind port (default 11435)")
    ap.add_argument("--history", type=int, default=50, help="max request history entries")
    args = ap.parse_args()

    port = prompt_for_port(args.port)

    ui = ConsoleUI()
    state = StubState(max_history=args.history)

    while True:
        try:
            httpd = run_server(state, ui, args.host, port)
            break
        except OSError as exc:
            if exc.errno == errno.EADDRINUSE:
                print(f"Port {port} is already in use.", flush=True)
                port = prompt_for_port(port)
                continue
            raise

    ui.set_server(args.host, port)
    ui.log("stub server started.")
    ui.log(f"listening on http://{args.host}:{port}/api/generate")
    ui.log("this is a local stub; it does not call real models.")

    try:
        console_loop(state, ui)
    finally:
        ui.log("shutting down server...")
        httpd.shutdown()


if __name__ == "__main__":
    main()
