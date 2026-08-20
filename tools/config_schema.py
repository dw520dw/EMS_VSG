# -*- coding: utf-8 -*-
"""
EMS_VSG 配置 schema —— json2xlsx / xlsx2json 共用。
与 config/modbus/ 下 JSON 的字段一一对应（对应 core/ModbusConfigLoader.cpp 的解析）。

字段元组: (列名, 类型, 约束)
  类型: str / int / float / bool / addr(地址，支持十进制或 "0x.." 字符串)
  约束: "" 可选; "required" 必填; "enum:a,b,c" 枚举
"""

# ---------- 设备（devices.json 的 device 对象）顶层字段 ----------
DEVICE_COLUMNS = [
    ("id",          "str", "required"),
    ("template",    "str", "required"),
    ("transport",   "str", "enum:rtu,tcp"),
    ("rtu_device",  "str", ""),
    ("rtu_baud",    "int", ""),
    ("tcp_ip",      "str", ""),
    ("tcp_port",    "int", ""),
    ("slave",       "int", ""),
    ("mysql_table", "str", ""),
    ("table_suffix", "int", ""),
    ("count_qt_addr", "int", ""),
    ("poll_ms",     "int", ""),
    ("inter_frame_ms", "int", ""),
    ("timeout_ms",  "int", ""),
    ("default_pt",  "int", ""),
    ("default_ct",  "int", ""),
]

# ---------- 设备嵌套对象：前缀展开成平铺列 ----------
# (json键, 子字段列表, 各子字段类型)
DEVICE_NESTED = [
    ("pt_ct_sync",
     ["enabled", "read_reg", "min_value", "max_value"],
     {"enabled": "bool", "read_reg": "int", "min_value": "int", "max_value": "int"}),
    ("comm",
     ["enabled", "probe_addr", "probe_count", "fail_threshold",
      "mysql_online_addr", "com_alarm_addr", "com_alarm_table"],
     {"enabled": "bool", "probe_addr": "int", "probe_count": "int", "fail_threshold": "int",
      "mysql_online_addr": "int", "com_alarm_addr": "int", "com_alarm_table": "str"}),
]

# ---------- 点表（templates/*.json 的 read.fields[] 每点字段，纯读侧） ----------
POINT_COLUMNS = [
    ("name",           "str",  ""),
    ("type",           "str",  "enum:u16,i16,u32_hi_lo,i32_hi_lo,u32_lo_hi,i32_lo_hi,bit,reg_bit,virtual_comm,virtual"),
    ("fc",             "int",  "enum:1,2,3,4"),
    ("address",        "addr", ""),
    ("cluster_stride", "int",  ""),
    ("bit",            "int",  ""),
    ("scale",          "float", ""),
    ("bias",           "float", ""),
    ("pt_exp",         "int",  ""),
    ("ct_exp",         "int",  ""),
]

# ---------- sink（config/modbus/sink/<template>.json）平铺列 ----------
# 「写库映射」: 每个写点一行（field_maps 的 {name,addr}）
SINK_COLUMNS = [
    ("template",   "str", "required"),
    ("field_map",  "str", "required"),
    ("name",       "str", "required"),
    ("addr",       "int", "required"),
]
# 「写库表路由」: 每个表路由一行（tables 的 mysql/mysql_prefix → field_map）
SINK_ROUTE_COLUMNS = [
    ("template",    "str", "required"),
    ("field_map",   "str", "required"),
    ("mysql",       "str", ""),
    ("mysql_prefix","str", ""),
]

# ---------- Excel 排版约定（json2xlsx / xlsx2json 必须一致） ----------
DEVICES_SHEET = "设备配置"
SINK_SHEET       = "写库映射"     # sink field_maps 平铺
SINK_ROUTE_SHEET = "写库表路由"   # sink tables 平铺
POINT_HEADER_ROW = 1   # 模板 Sheet 第1行: 点表表头（模板不再有 sink 头行）
POINT_START_ROW  = 2   # 第2行起: 点数据


# ---------- JSON 导出格式（点表/写库映射用紧凑一行一测点） ----------
def dump_config(obj):
    """点表风格 JSON：外层 key: { 同行开花，数组元素（点/写点）压成单行。

    与旧版手写模板格式一致（一行一个测点），便于阅读和 diff。引擎不关心格式。
    devices.json 仍用 json.dump(indent=2)（保持原多行格式）。
    """
    import json as _json

    lines = []

    def scalar(v):
        if v is None:
            return 'null'
        if isinstance(v, bool):
            return 'true' if v else 'false'
        return _json.dumps(v, ensure_ascii=False)

    def q(k):
        return _json.dumps(k, ensure_ascii=False)

    def _body(d, level):
        """输出 dict 的 keys；嵌套 dict 打开后在此递归并补闭合 '}'。
        顶层由调用方负责首尾 '{}'。"""
        keys = list(d.keys())
        for i, k in enumerate(keys):
            v = d[k]
            comma = ',' if i < len(keys) - 1 else ''
            pad = '  ' * level
            if isinstance(v, dict):
                if not v:
                    lines.append(pad + '%s: { }%s' % (q(k), comma))
                else:
                    lines.append(pad + '%s: {' % q(k))
                    _body(v, level + 1)
                    lines.append(pad + '}' + comma)  # 闭合在 key 的层级
            elif isinstance(v, list):
                if not v:
                    lines.append(pad + '%s: [ ]%s' % (q(k), comma))
                else:
                    lines.append(pad + '%s: [' % q(k))
                    for j, x in enumerate(v):
                        _elem(x, level + 1, j == len(v) - 1)
                    lines.append(pad + ']' + comma)
            else:
                lines.append(pad + '%s: %s%s' % (q(k), scalar(v), comma))

    def _elem(x, level, is_last):
        pad = '  ' * level
        if isinstance(x, dict):
            # 全标量 → 压成单行；含嵌套 → 回退多行
            if all(not isinstance(v, (dict, list)) for v in x.values()):
                inner = ', '.join('%s: %s' % (q(k), scalar(v)) for k, v in x.items())
                lines.append(pad + '{ ' + inner + ' }' + ('' if is_last else ','))
                return
            lines.append(pad + '{')
            _body(x, level + 1)
            lines[-1] += '' if is_last else ','
        elif isinstance(x, list):
            lines.append(pad + '[')
            for j, e in enumerate(x):
                _elem(e, level + 1, j == len(x) - 1)
            lines.append(pad + ']' + ('' if is_last else ','))
        else:
            lines.append(pad + scalar(x) + ('' if is_last else ','))

    lines.append('{')
    _body(obj, 1)
    lines.append('}')
    return '\n'.join(lines) + '\n'
