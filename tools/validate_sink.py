#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
validate_sink.py —— 校验分离后配置（devices.json + templates/* + sink/*）自洽性，
与 C++ 加载器（loadSinkWriteMap）的期望保持一致。

校验项：
  - 每台设备能路由到 field_map（精确表名 → 前缀 → 唯一 map 兜底）
  - 每个写点 name 存在于模板 read.fields、addr>=0、addr 不重复
  - 设备引用的 template 都有 sink 文件
  - （可选 --manifest）与迁移期产出的 sink_manifest.json 逐点比对

用法:
    python tools/validate_sink.py [--config config/modbus] [--manifest sink_manifest.json]
"""
import os
import sys
import json
import argparse


def _repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def resolve_field_map(device, sink, problems):
    """镜像 C++ loadSinkWriteMap 的路由规则，返回 field_map 名或 None。"""
    mysql = device.get("mysql_table")
    for t in sink.get("tables", []):
        if t.get("mysql") == mysql or t.get("mysql_prefix") == mysql:
            return t.get("field_map")
    maps = sink.get("field_maps", {})
    if len(maps) == 1:
        return next(iter(maps))
    problems.append("设备 %s 无法路由 field_map（tables 未匹配 mysql_table=%s 且 field_maps 非唯一）"
                    % (device.get("id"), mysql))
    return None


def main():
    ap = argparse.ArgumentParser(description="校验 devices.json/templates/sink 写库映射自洽")
    ap.add_argument("--config", default=None, help="config/modbus 目录，默认 <仓库>/config/modbus")
    ap.add_argument("--manifest", default=None, help="迁移期 sink_manifest.json，做逐点比对")
    args = ap.parse_args()

    cfg = args.config or os.path.join(_repo_root(), "config", "modbus")
    tpl_dir = os.path.join(cfg, "templates")
    sink_dir = os.path.join(cfg, "sink")

    with open(os.path.join(cfg, "devices.json"), encoding="utf-8") as f:
        devices = json.load(f)["devices"]

    templates = {}
    for fn in sorted(os.listdir(tpl_dir)):
        if fn.endswith(".json"):
            with open(os.path.join(tpl_dir, fn), encoding="utf-8") as f:
                templates[fn[:-5]] = json.load(f)
    sinks = {}
    for fn in sorted(os.listdir(sink_dir)):
        if fn.endswith(".json"):
            with open(os.path.join(sink_dir, fn), encoding="utf-8") as f:
                sinks[fn[:-5]] = json.load(f)

    problems = []
    warnings = []
    for d in devices:
        dev_id = d.get("id")
        tpl = d.get("template")
        if tpl not in templates:
            problems.append("设备 %s 引用了不存在的模板 %s" % (dev_id, tpl))
            continue
        if tpl not in sinks:
            problems.append("设备 %s 缺少 sink/%s.json（模板有读点但无写库映射）" % (dev_id, tpl))
            continue
        sink = sinks[tpl]
        map_name = resolve_field_map(d, sink, problems)
        if map_name is None or map_name not in sink.get("field_maps", {}):
            continue
        known = {f.get("name") for f in templates[tpl].get("read", {}).get("fields", []) if f.get("name")}
        seen_addr = set()
        for e in sink["field_maps"][map_name]:
            nm, ad = e.get("name"), e.get("addr")
            if nm not in known:
                problems.append("设备 %s 写点 %s 不在模板 %s read.fields 中" % (dev_id, nm, tpl))
            if ad is None or ad < 0:
                problems.append("设备 %s 写点 %s addr 非法: %r" % (dev_id, nm, ad))
            elif ad in seen_addr:
                problems.append("设备 %s 写点 addr 重复: %d (%s)" % (dev_id, ad, nm))
            seen_addr.add(ad)

    # manifest 逐点比对（迁移期审计）
    if args.manifest:
        with open(args.manifest, encoding="utf-8") as f:
            manifest = json.load(f)
        for d in devices:
            dev_id, tpl = d.get("id"), d.get("template")
            if tpl not in sinks:
                continue
            sink = sinks[tpl]
            map_name = resolve_field_map(d, sink, problems)
            if map_name is None:
                continue
            actual = {e.get("name"): e.get("addr") for e in sink["field_maps"].get(map_name, [])}
            expect = manifest.get(dev_id)
            if expect is None:
                warnings.append("manifest 无设备 %s 记录" % dev_id)
                continue
            if actual != expect:
                only_a = set(actual) - set(expect)
                only_e = set(expect) - set(actual)
                diff = [k for k in (set(actual) & set(expect)) if actual[k] != expect[k]]
                problems.append("设备 %s 写映射与 manifest 不一致（仅实际有:%s 仅manifest有:%s 值不同:%s）"
                                % (dev_id, sorted(only_a), sorted(only_e), diff))

    for w in warnings:
        print("[警告] %s" % w)
    if problems:
        print("发现 %d 处问题：" % len(problems))
        for p in problems:
            print("  [错误] %s" % p)
        sys.exit(1)
    print("校验通过：%d 台设备、%d 模板、%d 写库映射全部自洽。"
          % (len(devices), len(templates), len(sinks)))


if __name__ == "__main__":
    main()
