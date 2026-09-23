/**
 * helper 协议一致性 + 真 LibreOffice 行为测试。只用 Node 内置模块,`npm test` 即可跑。
 *
 * 需求:拆仓库之后,本仓库必须自己证明 helper 在每个平台上都守协议 v1、并且对真实
 * LibreOffice 的修改能保存、重开后还在。NextCoWork 宿主那边只拿构建产物,不再编译引擎。
 *
 * 需要两样东西,缺一样就整组**失败**(不是跳过 —— CI 上跳过等于没验证):
 *   - helper:`node scripts/build-helper.mjs` 的产物;
 *   - LibreOffice:环境变量 NCW_LO_PATH,指向 program 目录
 *     (macOS 是 LibreOffice.app/Contents/Frameworks)。
 * 本地想只跑其它东西时,设 NCW_SKIP_ENGINE_TESTS=1。
 */
import assert from 'node:assert/strict'
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { after, before, describe, test } from 'node:test'
import { fileURLToPath } from 'node:url'
import { HelperClient } from './protocol-client.mjs'
import { docxParagraphs } from './zip.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const exe = process.platform === 'win32' ? 'ncw-office-helper.exe' : 'ncw-office-helper'
const helper = join(root, 'native', `${process.platform}-${process.arch}`, exe)
const loPath = process.env.NCW_LO_PATH ?? (process.platform === 'darwin' ? '/Applications/LibreOffice.app/Contents/Frameworks' : '')
const skip = process.env.NCW_SKIP_ENGINE_TESTS === '1'
const TIMEOUT = 240_000

let work = ''
const clients = []

async function start() {
  const client = await HelperClient.start({ helper, loPath, workDir: join(work, `helper-${clients.length}`) })
  clients.push(client)
  return client
}

function fixture(kind, name) {
  const path = join(work, name)
  copyFileSync(join(root, 'test-fixtures', `blank.${kind}`), path)
  return path
}

describe('ncw-office-helper protocol v1 against a real LibreOffice', { skip }, () => {
  before(() => {
    assert.ok(existsSync(helper), `helper not built: ${helper} (run node scripts/build-helper.mjs)`)
    assert.ok(loPath !== '' && existsSync(loPath), `LibreOffice not found; set NCW_LO_PATH (got "${loPath}")`)
    work = mkdtempSync(join(tmpdir(), 'ncw-helper-conformance-'))
  })

  after(async () => {
    await Promise.all(clients.map((client) => client.close()))
    for (const client of clients) assert.equal(client.protocolViolation, undefined, String(client.protocolViolation))
    if (work !== '') rmSync(work, { recursive: true, force: true })
  })

  test('says hello with protocol 1 and a LibreOffice version, on a clean stdout', { timeout: TIMEOUT }, async () => {
    const client = await start()
    assert.equal(client.hello.protocol, 1)
    assert.match(client.hello.engineVersion, /LibreOffice/)
  })

  test('.docx: insert text, save, reopen in a fresh helper, read it back', { timeout: TIMEOUT }, async () => {
    const file = fixture('docx', 'report.docx')
    const out = join(work, 'report-saved.docx')
    const writer = await start()
    const opened = await writer.request('document.open', { path: file, format: 'docx' })
    assert.deepEqual(opened.capabilities.operations, ['text.insert', 'text.findReplace', 'paragraph.style', 'paragraph.insert'])
    assert.equal(opened.capabilities.canSave, true)
    await writer.request('document.apply', {
      operations: [{ kind: 'text.insert', target: { generation: 1, ref: 'document' }, position: 'end', text: '第一段 hello\n第二段 world' }]
    })
    await writer.request('document.saveAs', { path: out, format: 'docx' })

    const reader = await start()
    await reader.request('document.open', { path: out, format: 'docx' })
    const { text } = await reader.request('document.query', { kind: 'text' })
    assert.match(text, /第一段 hello/)
    assert.match(text, /第二段 world/)
  })

  test('.docx: find/replace, style by match and insert around an anchor, verified after reopen', { timeout: TIMEOUT }, async () => {
    const file = fixture('docx', 'contract.docx')
    const out = join(work, 'contract-saved.docx')
    const writer = await start()
    await writer.request('document.open', { path: file, format: 'docx' })
    await writer.request('document.apply', {
      operations: [{ kind: 'text.insert', target: { generation: 1, ref: 'document' }, position: 'end', text: '第一章 总则\n甲方 Alpha 签署\n甲方 alpha 付款 1.5 万\n附件 105 页' }]
    })
    const applied = await writer.request('document.apply', {
      operations: [
        { kind: 'text.findReplace', find: '甲方', replace: '客户', expectedCount: 2 },
        { kind: 'text.findReplace', find: 'Alpha', replace: 'ACME', matchCase: true },
        { kind: 'paragraph.style', find: '第一章', style: 'Heading 1', expectedCount: 1 },
        { kind: 'paragraph.insert', anchor: '付款', position: 'after', text: '新增条款 A\n新增条款 B' },
        { kind: 'paragraph.insert', anchor: '附件', position: 'before', text: '前置说明' }
      ]
    })
    assert.deepEqual(applied.results.map((r) => r.matches), [2, 1, 1, 1, 1])
    await writer.request('document.saveAs', { path: out, format: 'docx' })

    const reader = await start()
    await reader.request('document.open', { path: out, format: 'docx' })
    const { text } = await reader.request('document.query', { kind: 'text' })
    // 标题段在纯文本里会带编号/缩进前缀(空白),那不是文档内容,按行去掉首尾空白再比
    const lines = text.split(/\r?\n/).map((line) => line.trim()).filter((line) => line !== '')
    assert.deepEqual(lines, ['第一章 总则', '客户 ACME 签署', '客户 alpha 付款 1.5 万', '新增条款 A', '新增条款 B', '前置说明', '附件 105 页'])
    // 样式从文本里看不出来,直接看保存出的 document.xml
    const styled = docxParagraphs(out).filter((p) => p.text !== '')
    assert.equal(styled.find((p) => p.text === '第一章 总则')?.style, 'Heading1')
    assert.ok(styled.filter((p) => p.text !== '第一章 总则').every((p) => p.style !== 'Heading1'), JSON.stringify(styled))
  })

  test('.docx: search is literal, counts are enforced, unknown styles and ambiguous anchors are refused', { timeout: TIMEOUT }, async () => {
    const file = fixture('docx', 'guard.docx')
    const client = await start()
    await client.request('document.open', { path: file, format: 'docx' })
    await client.request('document.apply', {
      operations: [{ kind: 'text.insert', target: { generation: 1, ref: 'document' }, position: 'end', text: '价格 105 元\n备注 甲方\n备注 乙方' }]
    })
    const before = (await client.request('document.query', { kind: 'text' })).text
    // "1.5" 在正则下会命中 "105";字面查找必须零命中并整批拒绝
    await assert.rejects(client.request('document.apply', { operations: [{ kind: 'text.findReplace', find: '1.5', replace: 'x' }] }), (e) => e.code === 'invalid_operation')
    await assert.rejects(client.request('document.apply', { operations: [{ kind: 'text.findReplace', find: '备注', replace: 'x', expectedCount: 1 }] }), (e) => e.code === 'invalid_operation' && /found 2/.test(e.message))
    await assert.rejects(client.request('document.apply', { operations: [{ kind: 'paragraph.style', find: '价格', style: 'No Such Style' }] }), (e) => e.code === 'invalid_operation')
    await assert.rejects(client.request('document.apply', { operations: [{ kind: 'paragraph.insert', anchor: '备注', position: 'after', text: 'x' }] }), (e) => e.code === 'invalid_operation' && /exactly one/.test(e.message))
    const after = (await client.request('document.query', { kind: 'text' })).text
    assert.equal(after, before)
  })

  test('.xlsx: literal cells stay literal, formulas compute, sheets insert', { timeout: TIMEOUT }, async () => {
    const file = fixture('xlsx', 'budget.xlsx')
    const out = join(work, 'budget-saved.xlsx')
    const writer = await start()
    const opened = await writer.request('document.open', { path: file, format: 'xlsx' })
    const sheet = opened.partNames[0]
    await writer.request('document.apply', {
      operations: [
        { kind: 'cells.set', sheet, range: 'A1:B2', values: [[21, '007'], ['=not a formula', null]] },
        { kind: 'cells.formula', sheet, cell: 'C1', formula: 'A1*2' },
        { kind: 'sheet.insert', name: 'Data' }
      ]
    })
    await writer.request('document.saveAs', { path: out, format: 'xlsx' })

    const reader = await start()
    const reopened = await reader.request('document.open', { path: out, format: 'xlsx' })
    assert.ok(reopened.partNames.includes('Data'), JSON.stringify(reopened.partNames))
    const { text } = await reader.request('document.query', { kind: 'cells', sheet, range: 'A1:C2' })
    const rows = text.split(/\r?\n/).map((line) => line.split('\t'))
    assert.deepEqual(rows[0].slice(0, 3), ['21', '007', '42'])
    assert.equal(rows[1][0], '=not a formula')
  })

  test('.pptx: insert and reorder slides in one batch and keep them after save', { timeout: TIMEOUT }, async () => {
    const file = fixture('pptx', 'deck.pptx')
    const out = join(work, 'deck-saved.pptx')
    const writer = await start()
    await writer.request('document.open', { path: file, format: 'pptx' })
    await writer.request('document.apply', {
      operations: [{ kind: 'slide.insert', index: 1 }, { kind: 'slide.insert', index: 0 }, { kind: 'slide.move', from: 0, to: 2 }]
    })
    await writer.request('document.saveAs', { path: out, format: 'pptx' })

    const reader = await start()
    const reopened = await reader.request('document.open', { path: out, format: 'pptx' })
    assert.equal(reopened.parts, 3)
  })

  test('.pdf opens read-only: no operations and no in-place save', { timeout: TIMEOUT }, async () => {
    const file = fixture('pdf', 'scan.pdf')
    const before = readFileSync(file)
    const client = await start()
    const opened = await client.request('document.open', { path: file, format: 'pdf' })
    assert.equal(opened.documentType, 'drawing')
    assert.deepEqual(opened.capabilities.operations, [])
    assert.equal(opened.capabilities.canSave, false)
    assert.ok(readFileSync(file).equals(before))
  })

  test('rejects bad batches as a whole without changing the document', { timeout: TIMEOUT }, async () => {
    const file = fixture('pptx', 'reject.pptx')
    const client = await start()
    const opened = await client.request('document.open', { path: file, format: 'pptx' })
    // ★ 越界的 slide.move 若漏到引擎会直接把 LibreOffice 拖崩;必须在校验阶段挡住
    await assert.rejects(
      client.request('document.apply', { operations: [{ kind: 'slide.insert', index: 0 }, { kind: 'slide.move', from: 0, to: 9 }] }),
      (error) => error.code === 'invalid_operation'
    )
    await assert.rejects(
      client.request('document.apply', { operations: [{ kind: 'cells.set', sheet: 'S', range: 'A1', values: [[1]] }] }),
      (error) => error.code === 'unsupported_operation'
    )
    await assert.rejects(client.request('document.frobnicate', {}), (error) => error.code === 'invalid_operation')
    const outline = await client.request('document.query', { kind: 'outline' })
    assert.equal(outline.parts, opened.parts)
  })
})
