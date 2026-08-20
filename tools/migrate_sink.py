#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
migrate_sink.py —— 把"读/写耦合"的旧配置迁移为分离结构：
  - templates/<t>.json  只保留 read.fields（纯读寄存器地图）
  - sink/<t>.json        独立写库映射（field_maps 显式 {name,addr} + tables 路由）

用法:
    python tools/migrate_sink.py [--config config/modbus] [--out <staging目录>]
        [--dry-run] [--verify-history <history_config路径>] [--map-name <field_map名>]

行为:
  - 对每个模板，按旧引擎语义（sequential: base_addr+write_mysql 序列下标；
    explicit: db_addr）计算写序 {name, addr} —— 保证 MySQL addr 零漂移
  - 新模板 = 删掉每个 field 的 db_addr/write_mysql 键、删文件尾 sink
  - 新 sink  = field_maps + tables（多簇设备用 mysql_prefix 复用于所有簇）
  - --dry-run 只打印不写；写走 --out（不传则覆盖 config/modbus）
  - --verify-history 与历史上传 addr→EN 映射逐点比对（额外保险）
"""
import os
import sys
import json
import copy
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import config_schema as sch


def _repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def resolve_name(field, index):
    if isinstance(field, dict) and field.get("name"):
        return str(field["name"])
    return "_f%d" % index


def compute_write_list(old_fields, mode, base_addr):
    """镜像旧引擎 writeRealtime 语义（ModbusPollEngine.cpp:505-539），返回 [(name, addr), ...]"""
    out = []
    if mode == "sequential":
        seq = 0
        for i, f in enumerate(old_fields):
            if not f.get("write_mysql", True):
                continue
            addr = base_addr + seq
            seq += 1
            out.append((resolve_name(f, i), addr))
    else:  # explicit
        for i, f in enumerate(old_fields):
            if not f.get("write_mysql", True):
                continue
            addr = f.get("db_addr", -1)
            if addr is None or int(addr) < 0:
                continue
            out.append((resolve_name(f, i), int(addr)))
    return out


def strip_write_keys(field):
    f = copy.deepcopy(field)
    f.pop("db_addr", None)
    f.pop("write_mysql", None)
    return f


def build_tables(devices_for_tpl, map_name):
    """由引用该模板的设备生成 tables 路由：多簇 → mysql_prefix，否则 mysql。"""
    tables = []
    seen = set()
    for d in devices_for_tpl:
        if d.get("table_suffix", 0) > 0 or d.get("count_qt_addr", 0) > 0:
            key = ("p", d.get("mysql_table"))
            entry = {"mysql_prefix": d.get("mysql_table")}
        else:
            key = ("m", d.get("mysql_table"))
            entry = {"mysql": d.get("mysql_table")}
        if key in seen:
            continue
        seen.add(key)
        entry["field_map"] = map_name
        tables.append(entry)
    return tables


def load_history_maps(history_dir):
    """读 history_config/history_upload_*.json，返回 [(table/mysql_prefix, {en: addr}), ...]"""
    result = []
    for fn in sorted(os.listdir(history_dir)):
        if not fn.endswith(".json") or not fn.startswith("history_upload"):
            continue
        try:
            doc = json.load(open(os.path.join(history_dir, fn), encoding="utf-8"))
        except Exception:
            continue
        fms = doc.get("field_maps", {}) or {}
        maps = {}
        for mname, entries in fms.items():
            d = {}
            for e in entries or []:
                if "addr" in e and "en" in e:
                    d[e["en"]] = int(e["addr"])
                elif "addr_from" in e and "en_prefix" in e:
                    for a in range(int(e["addr_from"]), int(e.get("addr_to", e["addr_from"])) + 1):
                        d["%s%d" % (e["en_prefix"], a - int(e["addr_from"]) + 1)] = a
            maps[mname] = d
        for t in doc.get("tables", []) or []:
            if "mysql" in t and t.get("field_map") in maps:
                result.append(("mysql", t["mysql"], maps[t["field_map"]]))
            elif "mysql_prefix" in t and t.get("field_map") in maps:
                result.append(("prefix", t["mysql_prefix"], maps[t["field_map"]]))
    return result


def verify_history(per_device, history_maps):
    """每台设备 {name:addr} 与历史上传映射比对；返回错误列表。"""
    errors = []
    for kind, table, mapping in history_maps:
        dev = per_device.get(table)  # 表名==设备 mysql_table（或前缀）
        if dev is None:
            continue  # 历史上传里有本采集没有的表（如 data_total），跳过
        for en, addr in mapping.items():
            got = dev.get(en)
            if got is None:
                errors.append("历史[%s] %s 未出现在设备 %s 写映射中" % (table, en, table))
            elif got != addr:
                errors.append("历史[%s] %s addr=%d，设备写映射=%d" % (table, en, addr, got))
    return errors


def main():
    ap = argparse.ArgumentParser(description="旧耦合配置 → 分离(templates纯读 + sink独立写库)")
    ap.add_argument("--config", default=None, help="config/modbus 目录，默认 <仓库>/config/modbus")
    ap.add_argument("--out", default=None, help="输出目录，默认覆盖 --config")
    ap.add_argument("--dry-run", action="store_true", help="只打印清单不写文件")
    ap.add_argument("--verify-history", default=None, help="历史上传 history_config 目录，做 addr 交叉比对")
    ap.add_argument("--map-name", default=None, help="field_map 命名，默认 = 模板名")
    ap.add_argument("--write-manifest", default=None, help="额外输出每设备 {name:addr} manifest JSON")
    args = ap.parse_args()

    cfg_dir = args.config or os.path.join(_repo_root(), "config", "modbus")
    out_dir = args.out or cfg_dir
    tpl_dir = os.path.join(cfg_dir, "templates")

    with open(os.path.join(cfg_dir, "devices.json"), encoding="utf-8") as f:
        devices = json.load(f)["devices"]
    by_tpl = {}
    for d in devices:
        by_tpl.setdefault(d.get("template"), []).append(d)

    summary = {}
    problems = []
    per_device = {}
    generated_tpls = {}
    generated_sinks = {}

    for tname in sorted(os.listdir(tpl_dir)):
        if not tname.endswith(".json"):
            continue
        tkey = tname[:-5]
        with open(os.path.join(tpl_dir, tname), encoding="utf-8") as f:
            old = json.load(f)
        old_fields = old.get("read", {}).get("fields", [])
        sink = old.get("sink", {}) or {}
        mode = "sequential" if str(sink.get("mode", "explicit")).lower() in ("sequential", "seq") else "explicit"
        base = int(sink.get("base_addr", 0))
        write_list = compute_write_list(old_fields, mode, base)

        # 新模板（纯读）
        new_fields = [strip_write_keys(f) for f in old_fields]
        # 防御：新模板应无残留写侧键
        for f in new_fields:
            if "db_addr" in f or "write_mysql" in f:
                problems.append("[%s] 新模板残留写侧键" % tkey)
        generated_tpls[tkey] = {"read": {"fields": new_fields}}

        # 新 sink
        map_name = args.map_name or tkey
        devices_for_tpl = by_tpl.get(tkey, [])
        tables = build_tables(devices_for_tpl, map_name)
        if not tables:
            problems.append("[%s] 模板无设备引用，无法生成 tables 路由" % tkey)
        generated_sinks[tkey] = {
            "field_maps": {map_name: [{"name": n, "addr": a} for n, a in write_list]},
            "tables": tables,
        }

        # 每设备 {name:addr}（含多簇：所有簇共用同一 field_map）
        for d in devices_for_tpl:
            per_device[d.get("id")] = dict(write_list)
            # 表名路由：多簇设备把 mysql_table 也映射到同一份写映射（供历史比对）
            per_device.setdefault(d.get("mysql_table"), dict(write_list))

        summary[tkey] = {"mode": mode, "write_points": len(write_list), "tables": tables}

    # ---- 校验 ----
    # 1) 等价性：写点 name 必须存在于新模板 read.fields 的 name 集合
    for tkey, tpl in generated_tpls.items():
        names = {f.get("name") for f in tpl["read"]["fields"] if f.get("name")}
        for wp in generated_sinks[tkey]["field_maps"][(args.map_name or tkey)]:
            if wp["name"] not in names:
                problems.append("[%s] 写点 %s 不在模板 read.fields 中" % (tkey, wp["name"]))

    # 2) --verify-history 交叉核对
    if args.verify_history:
        hm = load_history_maps(args.verify_history)
        herr = verify_history(per_device, hm)
        problems.extend(herr)

    # ---- 输出 ----
    print("迁移预览（--dry-run 不写文件）：")
    for tkey, s in summary.items():
        print("  %-14s mode=%-11s 写点=%-4d tables=%s"
              % (tkey, s["mode"], s["write_points"],
                 ", ".join("(%s:%s)" % ("prefix" if "mysql_prefix" in t else "table",
                                        t.get("mysql") or t.get("mysql_prefix")) for t in s["tables"])))

    if problems:
        print("\n发现 %d 处问题，拒绝产出：" % len(problems))
        for p in problems:
            print("  [问题] %s" % p)
        sys.exit(1)

    if args.dry_run:
        print("\n[dry-run] 校验通过，未写任何文件。确认后去掉 --dry-run 生成。")
        return

    # ---- 写入（点表/sink 用紧凑一行一测点格式；devices.json 保持多行） ----
    os.makedirs(os.path.join(out_dir, "templates"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "sink"), exist_ok=True)
    with open(os.path.join(out_dir, "devices.json"), "w", encoding="utf-8") as f:
        f.write(json.dumps({"devices": devices}, ensure_ascii=False, indent=2) + "\n")
    for tkey, tpl in generated_tpls.items():
        with open(os.path.join(out_dir, "templates", tkey + ".json"), "w", encoding="utf-8") as f:
            f.write(sch.dump_config(tpl))
    for tkey, sink in generated_sinks.items():
        with open(os.path.join(out_dir, "sink", tkey + ".json"), "w", encoding="utf-8") as f:
            f.write(sch.dump_config(sink))

    if args.write_manifest:
        manifest = {d: {n: a for n, a in per_device[d].items()} for d in per_device
                    if d in {x["id"] for x in devices}}
        with open(args.write_manifest, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=2)

    print("\n已写入 %s：" % os.path.abspath(out_dir))
    print("  devices.json (内容不变) + %d 个模板(纯读) + %d 个 sink(独立写库)"
          % (len(generated_tpls), len(generated_sinks)))


if __name__ == "__main__":
    main()
