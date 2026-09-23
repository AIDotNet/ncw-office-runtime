/**
 * 协议 v1 的最小客户端 —— 只给一致性测试用,不依赖 NextCoWork 仓库。
 *
 * 需求:引擎(C++,本仓库)与宿主(TypeScript,NextCoWork 仓库)拆成两个仓库之后,
 * 两边只靠「协议 v1」连接。这个客户端按协议文档独立实现一遍分帧与请求配对,
 * 于是本仓库自己就能证明 helper 守约,而不必等宿主那边的 CI。
 *
 * 帧格式:[u32 BE 负载长度][u8 类型 0=JSON 1=二进制][负载]。与宿主的
 * src/main/document-engine/native-frame.ts 保持一致 —— 改了任一边,两边都要改,
 * 并提升 nativeComponents[].protocol。
 */
import { spawn } from 'node:child_process'
import { mkdirSync } from 'node:fs'
import { join } from 'node:path'

const MAX_FRAME_BYTES = 64 * 1024 * 1024

export class ProtocolError extends Error {}

export class HelperClient {
  constructor(child) {
    this.child = child
    this.pending = new Map()
    this.nextId = 1
    this.buffer = Buffer.alloc(0)
    this.stderr = ''
    this.hello = null
    this.exit = new Promise((resolve) => { child.on('exit', (code, signal) => { resolve({ code, signal }) }) })
    child.stderr.on('data', (chunk) => { this.stderr = (this.stderr + chunk.toString('utf8')).slice(-8000) })
    child.stdout.on('data', (chunk) => { this.onData(chunk) })
    this.exit.then(() => {
      for (const { reject } of this.pending.values()) reject(new ProtocolError(`helper exited\n${this.stderr}`))
      this.pending.clear()
    })
  }

  /**
   * 起 helper,等 hello。HOME/TMP 指向私有目录 —— 与宿主给 helper 的环境一致,
   * 否则 LibreOffice profile 会落到跑测试那个人的家目录里。
   */
  static async start({ helper, loPath, workDir, timeoutMs = 120_000 }) {
    mkdirSync(join(workDir, 'home'), { recursive: true })
    mkdirSync(join(workDir, 'tmp'), { recursive: true })
    const env = {
      HOME: join(workDir, 'home'),
      USERPROFILE: join(workDir, 'home'),
      TMPDIR: join(workDir, 'tmp'),
      TMP: join(workDir, 'tmp'),
      TEMP: join(workDir, 'tmp')
    }
    for (const key of ['PATH', 'SystemRoot', 'WINDIR', 'LANG', 'LC_ALL']) if (process.env[key] !== undefined) env[key] = process.env[key]
    const child = spawn(helper, [`--lo-path=${loPath}`], { cwd: workDir, env, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true })
    const client = new HelperClient(child)
    await client.waitHello(timeoutMs)
    return client
  }

  waitHello(timeoutMs) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { reject(new ProtocolError(`no hello within ${timeoutMs}ms\n${this.stderr}`)) }, timeoutMs)
      this.onHello = (data) => { clearTimeout(timer); resolve(data) }
      this.exit.then(({ code }) => { clearTimeout(timer); reject(new ProtocolError(`helper exited (${code}) before hello\n${this.stderr}`)) })
    })
  }

  onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk])
    while (this.buffer.length >= 5) {
      const length = this.buffer.readUInt32BE(0)
      const type = this.buffer.readUInt8(4)
      // 帧头坏了就是 stdout 被 LibreOffice 的日志污染了 —— helper 的不变式 1 被打破
      if (length > MAX_FRAME_BYTES || (type !== 0 && type !== 1)) {
        this.protocolViolation = new ProtocolError(`corrupt frame header (length=${length}, type=${type}); stdout was polluted`)
        this.child.kill('SIGKILL')
        return
      }
      if (this.buffer.length < 5 + length) return
      const payload = this.buffer.subarray(5, 5 + length)
      this.buffer = this.buffer.subarray(5 + length)
      if (type !== 0) continue
      let message
      try {
        message = JSON.parse(payload.toString('utf8'))
      } catch {
        this.protocolViolation = new ProtocolError('control frame is not valid JSON')
        this.child.kill('SIGKILL')
        return
      }
      if (message.v !== 1) continue
      if (message.event === 'hello') { this.hello = message.data; this.onHello?.(message.data); continue }
      const pending = this.pending.get(message.id)
      if (pending === undefined) continue
      this.pending.delete(message.id)
      if (message.ok === true) pending.resolve(message.result)
      else pending.reject(Object.assign(new Error(message.error?.message ?? 'error'), { code: message.error?.code }))
    }
  }

  request(method, params) {
    const id = this.nextId++
    const payload = Buffer.from(JSON.stringify({ v: 1, id, method, params }), 'utf8')
    const header = Buffer.alloc(5)
    header.writeUInt32BE(payload.length, 0)
    header.writeUInt8(0, 4)
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject })
      this.child.stdin.write(Buffer.concat([header, payload]))
    })
  }

  async close() {
    if (this.child.exitCode !== null || this.child.signalCode !== null) return
    await Promise.race([this.request('shutdown', {}).catch(() => undefined), new Promise((r) => { setTimeout(r, 5000) })])
    const exited = await Promise.race([this.exit.then(() => true), new Promise((r) => { setTimeout(() => { r(false) }, 5000) })])
    if (!exited) this.child.kill('SIGKILL')
  }
}
