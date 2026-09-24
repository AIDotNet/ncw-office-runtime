#!/usr/bin/env bash
# 跑一遍协议一致性测试;失败时把前几处失败写成 annotation。
#
# 需求:job 日志要登录才能下载,annotation 在公开 API 里可见 —— 不用拿凭据也能看到是哪一步、
# LibreOffice 报了什么。取「前几处失败」而不是日志尾部:尾部只剩级联超时,根因在第一处。
# 系统里的 LibreOffice 与随包的 LibreOffice 各跑一遍,所以抽成脚本,两个步骤共用。
#
# 用法:.github/scripts/conformance.sh "<annotation 标题>"
set +e
title="${1:-conformance output}"
log="$RUNNER_TEMP/conformance-$$.log"
npm test 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" != "0" ]; then
  grep -v Fontconfig "$log" | grep -A14 "not ok" | head -n 70 | TITLE="$title" node -e '
    let s = ""
    process.stdin.on("data", (d) => { s += d }).on("end", () => {
      const e = s.replace(/%/g, "%25").replace(/\r/g, "%0D").replace(/\n/g, "%0A")
      console.log(`::error title=${process.env.TITLE}::` + e)
    })'
fi
exit "$status"
