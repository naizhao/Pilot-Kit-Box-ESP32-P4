#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用 AEDT 原生脚本接口验证 HFSS Student 自动化链路。"""

import datetime
import json
import os
import traceback


WORK_DIR = os.path.join(os.path.expanduser("~"), "Documents", "HFSS_IFA_Codex")
PROJECT_FILE = os.path.join(WORK_DIR, "hfss_session_smoke.aedt")
STATUS_FILE = os.path.join(WORK_DIR, "hfss_session_smoke_status.json")


def write_status(**values):
    """写出机器可读的自检状态，便于通过 SSH 检查交互会话结果。"""
    if not os.path.isdir(WORK_DIR):
        os.makedirs(WORK_DIR)
    payload = {"updated_at": datetime.datetime.utcnow().isoformat() + "Z"}
    payload.update(values)
    with open(STATUS_FILE, "w") as status_handle:
        status_handle.write(json.dumps(payload, ensure_ascii=True, indent=2))


def main():
    write_status(state="starting", project=PROJECT_FILE)

    try:
        import ScriptEnv

        ScriptEnv.Initialize("Ansoft.ElectronicsDesktop")
        project = oDesktop.NewProject()  # noqa: F821 - 由 AEDT 注入。
        project.InsertDesign("HFSS", "HFSS_Smoke", "DrivenModal", "")
        project.SaveAs(PROJECT_FILE, True)
        write_status(
            state="ok",
            project=PROJECT_FILE,
            design="HFSS_Smoke",
        )
        return 0
    except Exception as exc:  # 状态文件必须保留完整诊断信息。
        write_status(
            state="error",
            project=str(PROJECT_FILE),
            error=repr(exc),
            traceback=traceback.format_exc(),
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
