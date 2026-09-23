#!/usr/bin/env node
/**
 * 把 native/<platform>-<arch>/target.json 汇总进 package.json 的 nativeComponents.targets。
 *
 * 需求:四个平台的 helper 在四台不同的 CI 机器上编译,每台只知道自己那一份摘要。
 * 发布用的插件包必须同时列出全部平台 —— NextCoWork 安装器会检查**每个**声明的
 * 平台入口都在包里,缺一个就整包拒装。所以汇总以「盘上实际存在的 target.json」为准,
 * 并在登记前核对摘要:target.json 和二进制对不上就失败,不把错的摘要写进清单。
 *
 * 用法:node scripts/register-targets.mjs     (也被 build-helper.mjs 调用)
 *
 * ★ 清单里的 targets 被**整表替换**成盘上找到的那些:没有构建产物的平台不会残留
 *   一条旧摘要。残留的话,安装器会因为入口缺失拒装整个包。
 */
import { createHash } from 'node:crypto'
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const ENTRY_RE = /^native\/(darwin|linux|win32)-(x64|arm64)\/ncw-office-helper(\.exe)?$/

export function registerTargets(pluginRoot) {
  const nativeRoot = join(pluginRoot, 'native')
  const targets = []
  if (existsSync(nativeRoot)) {
    for (const dir of readdirSync(nativeRoot, { withFileTypes: true })) {
      if (!dir.isDirectory()) continue
      const descriptor = join(nativeRoot, dir.name, 'target.json')
      if (!existsSync(descriptor)) continue
      const target = JSON.parse(readFileSync(descriptor, 'utf8'))
      if (!ENTRY_RE.test(target.entry) || !target.entry.startsWith(`native/${target.platform}-${target.arch}/`)) {
        throw new Error(`${descriptor}: unexpected entry ${target.entry}`)
      }
      const binary = join(pluginRoot, target.entry)
      const actual = createHash('sha256').update(readFileSync(binary)).digest('hex')
      if (actual !== target.sha256) throw new Error(`${descriptor}: sha256 does not match ${target.entry}`)
      targets.push({ platform: target.platform, arch: target.arch, entry: target.entry, sha256: target.sha256 })
    }
  }
  targets.sort((a, b) => `${a.platform}-${a.arch}`.localeCompare(`${b.platform}-${b.arch}`))

  const manifestPath = join(pluginRoot, 'package.json')
  const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'))
  const component = manifest.nativeComponents?.find((c) => c.id === 'libreoffice')
  if (component === undefined) throw new Error('package.json has no nativeComponents entry with id "libreoffice"')
  component.targets = targets
  writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`)
  return targets.map((t) => `${t.platform}-${t.arch}`)
}

if (process.argv[1] !== undefined && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
  const registered = registerTargets(root)
  if (registered.length === 0) {
    console.error('✗ no native/<platform>-<arch>/target.json found; build the helper first')
    process.exit(1)
  }
  console.log(`✓ registered ${registered.join(', ')}`)
}
