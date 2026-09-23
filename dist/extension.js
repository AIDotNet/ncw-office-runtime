/**
 * 逻辑侧 —— 这个插件几乎不需要它。
 *
 * ncw.office-runtime 只提供文档引擎(`contributes.documentEngines` + `nativeComponents`),
 * 引擎进程由宿主核对摘要后启动(src/main/document-engine/native-host.ts),插件代码
 * 不参与。`kind: "extension"` 的清单必须有 `main`,所以这里是一个有意为空的入口。
 */
export function activate() {
  // 有意为空:引擎的生命周期由宿主的文档会话管理
}

export function deactivate() {
  // 同上
}
