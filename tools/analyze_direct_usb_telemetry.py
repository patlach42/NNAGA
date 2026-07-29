#!/usr/bin/env python3
"""Analyze DirectUsbDeviceStressTest TELEMETRY logcat records."""
import argparse
import json
import re
import statistics
import sys
from collections import defaultdict

TOKEN = re.compile(r"(?P<key>[A-Za-z_][\w.-]*)=(?P<value>\"[^\"]*\"|'[^']*'|[^\s]+)")
CASE_KEYS = ("rate", "bits", "bytes", "channels", "buffer", "multiplier")
NUMERIC = re.compile(r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$")
PRIMARY_REQUIRED = ("cycle", "state", "failure", "actual_xrun_growth", "capture_transfer_errors",
                    "playback_transfer_errors", "lifecycle_failures", "transport_failed",
                    "deadline_miss_growth", "last_dsp_ns", "peak_dsp_ns", "last_cycle_ns",
                    "peak_cycle_ns", "deadline_budget_ns", "known_host_latency_frames")
LIFECYCLE_REQUIRED = ("lifecycle_after_stop", "state", "failure", "lifecycle_failures")


def parse_value(value):
    value = value.strip("\"'")
    if value.lower() in ("true", "false"):
        return value.lower() == "true"
    if NUMERIC.match(value):
        try:
            n = float(value)
            return int(n) if n.is_integer() else n
        except ValueError:
            pass
    return value


def records_from(streams, parse_diagnostics=None, audit_summaries=None):
    records = []
    errors = parse_diagnostics if parse_diagnostics is not None else []
    summaries = audit_summaries if audit_summaries is not None else []
    for name, stream in streams:
        for lineno, line in enumerate(stream, 1):
            summary_marker = line.find("AUDIT_SUMMARY ")
            if summary_marker >= 0:
                summary = {m.group("key"): parse_value(m.group("value"))
                           for m in TOKEN.finditer(line[summary_marker + len("AUDIT_SUMMARY "):])}
                summary["_source"], summary["_line"] = name, lineno
                summaries.append(summary)
            marker = line.find("TELEMETRY reason=")
            if marker < 0:
                continue
            fields = {m.group("key"): parse_value(m.group("value"))
                      for m in TOKEN.finditer(line[marker + len("TELEMETRY"):])}
            fields["_source"], fields["_line"] = name, lineno
            reason = str(fields.get("reason", "")).lower()
            required = CASE_KEYS + ("reason", "schema") + (LIFECYCLE_REQUIRED if reason.startswith("lifecycle-after-stop") else PRIMARY_REQUIRED)
            missing = [key for key in required if key not in fields]
            schema = fields.get("schema")
            if reason == "lifecycle-after-stop" and (fields.get("lifecycle_after_stop") != 1 or str(fields.get("state", "")).lower() not in ("stopped", "0") or str(fields.get("failure", "")).lower() not in ("ok", "0")):
                missing.append("valid lifecycle-after-stop status")
            if missing:
                errors.append("%s:%d malformed TELEMETRY missing=%s" % (name, lineno, ",".join(missing)))
            elif not isinstance(schema, int) or isinstance(schema, bool) or schema not in (3, 4, 5):
                errors.append("%s:%d unsupported TELEMETRY schema=%r (expected 3, 4, or 5)" % (name, lineno, schema))
            else:
                records.append(fields)
    return records


def number(record, *names):
    for name in names:
        value = record.get(name)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return float(value)
    return None


def _int(record, *names):
    value = number(record, *names)
    return int(value) if value is not None else 0
def _max_int(record, *names):
    values = [number(record, name) for name in names]
    values = [int(value) for value in values if value is not None]
    return max(values, default=0)




def _stable_pass(record):
    reason = str(record.get("reason", "")).lower()
    if reason != "pass":
        return False
    if str(record.get("state", "")).lower() not in ("running", "1") or str(record.get("failure", "")).lower() not in ("ok", "0"):
        return False
    budget = number(record, "deadline_budget_ns")
    last_cycle = number(record, "last_cycle_ns")
    host_frames = number(record, "known_host_latency_frames")
    peak_cycle = number(record, "peak_cycle_ns")
    if budget is None or budget <= 0 or last_cycle is None or last_cycle <= 0 or host_frames is None or host_frames <= 0 or peak_cycle is None or peak_cycle < last_cycle:
        return False
    if _int(record, "actual_xrun_growth", "xrun_growth") != 0:
        return False
    if _max_int(record, "capture_transfer_errors", "playback_transfer_errors", "transfer_errors") != 0:
        return False
    if _int(record, "lifecycle_failures") != 0 or _int(record, "transport_failed") != 0:
        return False
    growth = number(record, "deadline_miss_growth")
    if (int(growth) if growth is not None else _int(record, "deadline_misses")) != 0:
        return False
    return True


def analyze(records, required, parse_diagnostics=None, audit_summaries=None):
    # Group by format/multiplier, then by buffer; retain all records for diagnostics.
    groups = defaultdict(lambda: defaultdict(list))
    for record in records:
        format_key = tuple(record.get(k) for k in ("rate", "bits", "bytes", "channels", "multiplier"))
        groups[format_key][record.get("buffer")].append(record)

    stable_groups, failures, failed_below = [], [], 0
    for format_key in sorted(groups, key=lambda key: tuple((v is None, v) for v in key)):
        buffers = groups[format_key]
        complete, statuses = [], {}
        for buffer, recs in buffers.items():
            passes = [r for r in recs if _stable_pass(r)]
            cycles = {r.get("cycle") for r in passes}
            bad_pass = [r for r in recs if str(r.get("reason", "")).lower() == "pass" and not _stable_pass(r)]
            lifecycle = [r for r in recs if str(r.get("reason", "")).lower() == "lifecycle-after-stop"]
            lifecycle_cycles = {r.get("cycle") for r in lifecycle
                                if _int(r, "lifecycle_after_stop") == 1 and
                                _int(r, "lifecycle_failures") == 0}
            lifecycle_ok = not lifecycle or all(c in lifecycle_cycles for c in cycles)
            complete_here = len(cycles) >= required and not bad_pass and lifecycle_ok
            statuses[buffer] = complete_here
            if complete_here:
                complete.append(buffer)

        numeric_complete = [b for b in complete if isinstance(b, (int, float)) and not isinstance(b, bool)]
        if not numeric_complete:
            failures.append("no complete stable buffer group=%s" % (format_key,))
            continue
        minimum = min(numeric_complete)
        bad_at_or_above = [b for b, ok in statuses.items() if isinstance(b, (int, float)) and b >= minimum and not ok]
        below = sum(1 for b, ok in statuses.items() if isinstance(b, (int, float)) and b < minimum and not ok)
        failed_below += below
        if bad_at_or_above:
            failures.append("non-monotonic/missing stable buffers group=%s buffers=%s" % (format_key, sorted(bad_at_or_above)))
        complete_sorted = sorted(numeric_complete)
        stable_groups.append({"rate": format_key[0], "bits": format_key[1], "bytes": format_key[2],
                              "channels": format_key[3], "multiplier": format_key[4],
                              "minimum_buffer": minimum, "complete_buffers": complete_sorted,
                              "failed_below_threshold": below})

    minimums = {
        (group["rate"], group["bits"], group["bytes"], group["channels"], group["multiplier"]): group["minimum_buffer"]
        for group in stable_groups
    }
    def at_or_above_threshold(record):
        format_key = tuple(record.get(k) for k in ("rate", "bits", "bytes", "channels", "multiplier"))
        buffer = record.get("buffer")
        minimum = minimums.get(format_key)
        return minimum is not None and isinstance(buffer, (int, float)) and not isinstance(buffer, bool) and buffer >= minimum

    critical_records = [r for r in records if at_or_above_threshold(r)]
    pass_records = [r for r in critical_records if str(r.get("reason", "")).lower() == "pass"]
    latencies = []
    for record in pass_records:
        rate, frames = number(record, "rate"), number(record, "known_host_latency_frames")
        if rate and frames is not None:
            latencies.append(frames / rate * 1000.0)
    dsp_last = next((number(r, "last_dsp_ns") for r in reversed(pass_records) if number(r, "last_dsp_ns") is not None), None)
    dsp_values = [number(r, "peak_dsp_ns") for r in pass_records if number(r, "peak_dsp_ns") is not None]
    cycle_values = [number(r, "last_cycle_ns") for r in pass_records if number(r, "last_cycle_ns") is not None]
    peak_cycle_values = [number(r, "peak_cycle_ns") for r in pass_records if number(r, "peak_cycle_ns") is not None]
    budgets = [number(r, "deadline_budget_ns") for r in pass_records if number(r, "deadline_budget_ns") is not None]
    deadline_misses = sum(_int(r, "deadline_miss_growth", "deadline_misses") for r in pass_records)
    lifecycle = max((_int(r, "lifecycle_failures") for r in critical_records), default=0)
    transfer = max((_max_int(r, "capture_transfer_errors", "playback_transfer_errors", "transfer_errors") for r in critical_records), default=0)
    xruns = sum(_int(r, "actual_xrun_growth", "xrun_growth", "actual_xruns", "playback_xruns", "xruns", "xrun") for r in critical_records)
    timing = {"last_cycle_ns": cycle_values[-1] if cycle_values else None,
              "peak_cycle_ns": max(peak_cycle_values) if peak_cycle_values else None,
              "deadline_budget_ns": budgets[-1] if budgets else None,
              "deadline_misses": deadline_misses}
    summary_errors = []
    if not audit_summaries:
        summary_errors.append("missing AUDIT_SUMMARY")
    else:
        summary = audit_summaries[-1]
        if summary.get("overall") != 1 or str(summary.get("result", "")).upper() != "PASS":
            summary_errors.append("AUDIT_SUMMARY overall/result is not PASS")
        summary_cycles = summary.get("cycles")
        if (not isinstance(summary_cycles, int) or isinstance(summary_cycles, bool)
                or not 2 <= summary_cycles <= 8):
            summary_errors.append("AUDIT_SUMMARY cycles=%r expected 2..8" % summary_cycles)
        else:
            for case_summary in audit_summaries:
                required_cycles = case_summary.get("required_cycles")
                if required_cycles is not None and required_cycles != summary_cycles:
                    summary_errors.append("AUDIT_SUMMARY required_cycles=%r expected=%d" % (required_cycles, summary_cycles))
                    break
        primary = [r for r in records if str(r.get("reason", "")).lower() not in ("lifecycle-after-stop", "lifecycle-after-stop-failed")]
        unique = {(tuple(r.get(k) for k in CASE_KEYS), r.get("cycle")) for r in primary}
        if summary.get("cases") != len(unique):
            summary_errors.append("AUDIT_SUMMARY cases=%r observed=%d" % (summary.get("cases"), len(unique)))
        for key, cycle in unique:
            if not any(tuple(r.get(k) for k in CASE_KEYS) == key and r.get("cycle") == cycle and str(r.get("reason", "")).lower() == "lifecycle-after-stop" and _int(r, "lifecycle_after_stop") != 0 for r in records):
                summary_errors.append("missing lifecycle-after-stop case=%s cycle=%s" % (key, cycle))
    diagnostics = list(parse_diagnostics or []) + summary_errors + list(failures)
    if lifecycle: diagnostics.append("lifecycle_failures=%d" % lifecycle)
    if transfer: diagnostics.append("transfer_errors=%d" % transfer)
    if xruns: diagnostics.append("xruns=%d" % xruns)
    if deadline_misses: diagnostics.append("deadline_misses=%d" % deadline_misses)
    result = {"stable_groups": stable_groups, "failed_below_threshold": failed_below,
              "latency_ms_estimate": ({"min": min(latencies), "median": statistics.median(latencies), "max": max(latencies)} if latencies else None),
              "estimated_host_queue_latency_ms": ({"min": min(latencies), "median": statistics.median(latencies), "max": max(latencies)} if latencies else None),
              "dsp": {"last_ns": dsp_last, "peak_ns": max(dsp_values) if dsp_values else None}, **timing,
              "failures": {"cases": failures, "xrun": xruns, "transfer": transfer, "lifecycle": lifecycle, "deadline_misses": deadline_misses},
              "record_count": len(records), "complete_stable_groups": len(stable_groups), "ok": bool(stable_groups) and not diagnostics}
    return result, diagnostics


def main(argv=None):
    ap = argparse.ArgumentParser(description="Analyze Direct USB TELEMETRY logcat records")
    ap.add_argument("files", nargs="*", help="log files (default: stdin; '-' also means stdin)")
    ap.add_argument("--json", metavar="PATH", help="write machine JSON to PATH, or '-' for stdout")
    ap.add_argument("--required-cycles", type=int, default=2, help="unique passing cycles required per buffer (default: 2)")
    args = ap.parse_args(argv)
    if args.required_cycles < 1:
        ap.error("--required-cycles must be >= 1")
    streams, handles, parse_diagnostics = [], [], []
    try:
        if not args.files or "-" in args.files:
            streams.append(("<stdin>", sys.stdin))
        for path in args.files:
            if path == "-":
                continue
            handle = open(path, encoding="utf-8", errors="replace")
            handles.append(handle); streams.append((path, handle))
        audit_summaries = []
        records = records_from(streams, parse_diagnostics, audit_summaries)
    finally:
        for handle in handles: handle.close()
    result, diagnostics = analyze(records, args.required_cycles, parse_diagnostics, audit_summaries)
    result["metadata"] = {"analog_loopback_measured": False, "latency_label": "estimated live host end-to-end queue latency (not analog loopback)", "required_cycles": args.required_cycles, "raw_records": records}
    payload = json.dumps(result, indent=2, sort_keys=True)
    if args.json:
        if args.json == "-": print(payload)
        else:
            with open(args.json, "w", encoding="utf-8") as out: out.write(payload + "\n")
    human = sys.stderr if args.json == "-" else sys.stdout
    print("Direct USB telemetry: %s (records=%d, stable groups=%d)" % ("PASS" if result["ok"] else "FAIL", result["record_count"], result["complete_stable_groups"]), file=human)
    print("Latency: estimated host queue ms (not analog loopback): %s" % (result["latency_ms_estimate"] or "n/a"), file=human)
    for group in result["stable_groups"]:
        print("Stable rate=%s bits=%s bytes=%s channels=%s multiplier=%s minimum_buffer=%s" % tuple(group[k] for k in ("rate", "bits", "bytes", "channels", "multiplier", "minimum_buffer")), file=human)
    print("DSP ns: last=%s peak=%s; cycle ns: last=%s peak=%s budget=%s; deadline misses=%d; failures xrun=%d transfer=%d lifecycle=%d; failed below threshold=%d" % (result["dsp"]["last_ns"], result["dsp"]["peak_ns"], result["last_cycle_ns"], result["peak_cycle_ns"], result["deadline_budget_ns"], result["deadline_misses"], result["failures"]["xrun"], result["failures"]["transfer"], result["failures"]["lifecycle"], result["failed_below_threshold"]), file=human)
    if diagnostics: print("Diagnostics: " + "; ".join(diagnostics), file=sys.stderr)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
