#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ci/gen_changelog.py 的自测。

刻意写成「一次性脚本」而不是测试框架用例：本仓库的单元测试全在 C++ 侧
（test/ 下五个套件），为 Python 脚本引入 pytest 依赖并不划算；而
gen_changelog.py 本身是 CI 闸门的一部分，闸门必须先被验证过。

做法是在临时 git 仓库里造出各种真实形态（无 tag、无提交、trailer 覆盖、
维护提交、版本提升提交、漂移、非 UTF-8 文件、PR 合并检出），逐个断言。
跑法：

    python ci/test_gen_changelog.py

CI 的 changelog job 会先跑本脚本再跑 --check：工具本身坏了的时候，
闸门必须先报「工具坏了」，而不是报「CHANGELOG 漂移」这种误导性结论。

仅依赖 Python 3 标准库。
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'gen_changelog.py')


class Repo(object):
    """临时 git 仓库 + gen_changelog 调用的薄封装。"""

    def __init__(self, prefix):
        self.path = tempfile.mkdtemp(prefix=prefix)

    def close(self):
        shutil.rmtree(self.path, ignore_errors=True)

    def git(self, *args):
        p = subprocess.run(['git'] + list(args), cwd=self.path,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert p.returncode == 0, p.stderr.decode('utf-8', 'replace')
        return p.stdout.decode('utf-8', 'replace')

    def init(self):
        self.git('init', '-q')
        self.git('config', 'user.email', 'ci@example.com')
        self.git('config', 'user.name', 'ci')
        self.git('config', 'commit.gpgsign', 'false')

    def commit(self, msg, fname='a.txt'):
        """在 fname 上追加一行并提交；不同分支用不同文件避免切换冲突。"""
        with open(os.path.join(self.path, fname), 'a',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        self.git('add', '.')
        self.git('commit', '-qm', msg)

    def commit_all(self, msg):
        self.git('add', '.')
        self.git('commit', '-qm', msg)

    def changelog_path(self):
        return os.path.join(self.path, 'CHANGELOG.md')

    def run(self, *args):
        argv = [sys.executable, TOOL] + list(args) + \
            ['--path', self.changelog_path()]
        p = subprocess.run(argv, cwd=self.path, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        return (p.returncode, p.stdout.decode('utf-8', 'replace'),
                p.stderr.decode('utf-8', 'replace'))

    def sync(self):
        rc, out, err = self.run()
        assert rc == 0, (rc, out, err)
        return out

    def check(self, *extra):
        return self.run('--check', *extra)

    def read(self):
        with open(self.changelog_path(), encoding='utf-8') as handle:
            return handle.read()

    def write(self, text):
        with open(self.changelog_path(), 'w', encoding='utf-8',
                  newline='') as handle:
            handle.write(text)


def case_classification():
    """分类规则、trailer 覆盖/跳过、维护提交过滤、版本提升报告。"""
    r = Repo('gen-changelog-class-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        r.write('# 更新日志\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')

        # --- 无 tag：清晰报错，不是 traceback ---
        rc, out, err = r.run('--print')
        assert rc == 2, (rc, out, err)
        assert '还没有任何 tag' in err and 'Traceback' not in err, err

        r.git('tag', 'v1.0.0')
        rc, out, err = r.run('--print')
        assert rc == 0 and '（暂无提交）' in out, (rc, out, err)

        r.commit('Add widget module')
        r.commit('Fix off-by-one in widget')
        r.commit('Make widget faster')
        r.commit('Document widget internals')                # 内部
        r.commit('Sweep away leftovers\n\nCategory: 修复\n')  # trailer 覆盖分类
        r.commit('Noise commit\n\nChangelog-Skip: yes\n')    # trailer 跳过
        r.commit('Bump version to 1.1.0')                    # 只报告版本

        # 只改 CHANGELOG.md 与工具自身 = 维护提交，必须被忽略
        os.makedirs(os.path.join(r.path, 'ci'), exist_ok=True)
        shutil.copy(TOOL, os.path.join(r.path, 'ci', 'gen_changelog.py'))
        r.git('add', '.')
        r.git('commit', '-qm', 'Touch changelog and tool only')

        rc, out, err = r.run('--print', '--until', 'HEAD')
        assert rc == 0, (rc, out, err)
        for want in ('Add widget module', 'Fix off-by-one in widget',
                     'Make widget faster', 'Document widget internals',
                     'Sweep away leftovers'):
            assert want in out, (want, out)
        for unwanted in ('Noise commit', 'Touch changelog and tool only',
                         'Bump version to 1.1.0'):
            assert unwanted not in out, (unwanted, out)
        assert '下一版本：1.1.0' in out, out
        for heading in ('#### 新增', '#### 变更', '#### 修复', '#### 内部'):
            assert out.count(heading) == 1, (heading, out)
        changed = out.split('#### 变更')[1].split('#### 修复')[0]
        fixed = out.split('#### 修复')[1].split('#### 内部')[0]
        assert 'Make widget faster' in changed, out
        assert 'Fix off-by-one in widget' in fixed, out
        # trailer 覆盖必须真的生效：'Sweep' 首词匹配不到规则，默认会落进「变更」
        assert 'Sweep away leftovers' in fixed, out
        assert 'Sweep away leftovers' not in changed, out
    finally:
        r.close()


def case_idempotent_write():
    """首次写入、幂等、标记缺失时追加。"""
    r = Repo('gen-changelog-write-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        r.write('# 更新日志\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')
        r.git('tag', 'v1.0.0')
        r.commit('Add widget module')

        out = r.sync()
        assert '已追加提交清单' in out, out
        first = r.read()
        assert r.sync() == '', '第二次刷新应当无事可做'
        assert r.read() == first, 'not idempotent'

        r.write('# 空\n')
        rc, out, err = r.check()
        assert rc == 1 and 'Traceback' not in err, (rc, err)
        assert '已追加提交清单' in r.sync(), '标记缺失时应追加'
        assert r.read().count('BEGIN generated') == 1
    finally:
        r.close()


def case_drift_gate():
    """固有一步延迟、漂移检测、刷新后复检。"""
    r = Repo('gen-changelog-drift-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        r.write('# 更新日志\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')
        r.git('tag', 'v1.0.0')

        r.commit('Add first widget')
        r.sync()
        assert r.check()[0] == 0, '刷新后必须自洽'
        assert 'Add first widget' not in r.read(), 'tip commit 不能收录自身'

        # 提交了下一条而没跟刷新：缺口落进区间 → 闸门必须报漂移
        r.commit('Add second widget')
        rc, out, err = r.check()
        assert rc == 1, 'drift not detected'
        assert 'python ci/gen_changelog.py' in err, err
        # 显式覆盖到 HEAD 时，缺口应恰好是 tip 那一条
        rc, out, err = r.check('--until', 'HEAD')
        assert rc == 1, 'gap up to HEAD must be reported'

        r.sync()
        body = r.read()
        assert 'Add first widget' in body, body
        assert 'Add second widget' not in body, 'tip commit must be excluded'
        assert r.check()[0] == 0, 'refreshed block must pass'

        # 再来一轮：确认闸门反复有效，而不是只有第一次有效
        r.commit('Add third widget')
        assert r.check()[0] == 1, 'drift must be re-detected'
        r.sync()
        body = r.read()
        assert 'Add second widget' in body and 'Add third widget' not in body, body
        assert r.check()[0] == 0
    finally:
        r.close()


def case_non_utf8():
    """非 UTF-8 文件应给出清晰报错，而不是抛 traceback。"""
    r = Repo('gen-changelog-encoding-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        with open(r.changelog_path(), 'wb') as handle:
            handle.write(b'# \xff\xfe bad\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')
        r.git('tag', 'v1.0.0')
        # HEAD 不能恰好落在 tag 上，否则工具会走「已冻结」跳过分支
        r.commit('Add something else', 'b.txt')
        rc, out, err = r.run()
        assert rc == 2, (rc, err)
        assert 'UTF-8' in err and 'Traceback' not in err, err
    finally:
        r.close()


def case_pr_merge_checkout():
    """PR 检出拿到的是 refs/pull/N/merge，区间终点必须退回 HEAD^2~1。

    历史（刻意做得短到能手推）：

        init ─ tag v1.0.0
        A  Add mainline work
        S  Add changelog skeleton    只改 CHANGELOG.md = 维护提交，被忽略
        ├─ feature ─ B  Add feature flag
        └─ C  Add other mainline work
        M  Merge pull request #1     父提交 C 与 B

    刷在 HEAD = S 上，终点是 S~1 = A，所以清单已含 A。

    检 M 时：content_tip = HEAD^2 = B，终点 B~1 = S。区间 v1.0.0..S 里只有 A
    与维护提交 S，渲染与清单相同 → 通过。若按 HEAD~1（= C）校验，C 不在
    清单里 → 误报漂移。
    """
    r = Repo('gen-changelog-pr-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        r.write('# 更新日志\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')
        r.git('tag', 'v1.0.0')

        r.commit('Add mainline work')            # A
        r.write('# 更新日志\n\n<!-- 骨架 -->\n')
        r.commit_all('Add changelog skeleton')  # S，维护提交，被忽略
        # HEAD 是 S，终点是 S~1 = A，于是清单已含 A
        r.sync()
        assert 'Add mainline work' in r.read()

        r.git('checkout', '-q', '-b', 'feature')
        r.commit('Add feature flag', 'b.txt')
        r.git('checkout', '-q', '-')
        r.commit('Add other mainline work', 'c.txt')
        r.git('merge', '--no-ff', '-q', '-m', 'Merge pull request #1', 'feature')
        assert len(r.git('rev-list', '--parents', '-n', '1',
                         'HEAD').split()) == 3, 'expected a merge commit'

        rc, out, err = r.check()
        assert rc == 0, ('PR merge checkout must pass', rc, err)
        rc, out, err = r.check('--until', 'HEAD~1')
        assert rc == 1, 'naive HEAD~1 must be the wrong answer'

        # 复检：合并检出下 sync 与 check 用同一个终点，B 与 C 由下一轮补上
        r.sync()
        body = r.read()
        assert r.check()[0] == 0, 'refreshed block must pass'
        assert 'Add mainline work' in body, body
        assert 'Add feature flag' not in body, 'tip commit 不能收录自身'
        assert 'Add other mainline work' not in body, 'mainline 新提交等下一轮'
    finally:
        r.close()


def case_head_on_tag():
    """打 tag 的那一提交上，校验与刷新都应跳过。

    那一刻「未发布」段已经被冻结成正式版本段落，区间起点就是 HEAD 自己，
    任何清单都必然对不上——那是假漂移，不能让它把发布提交卡在红上。
    """
    r = Repo('gen-changelog-tag-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        r.write('# 更新日志\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')
        r.git('tag', 'v1.0.0')
        r.commit('Add widget module')
        # HEAD 就是这条提交，所以清单还不含它（固有一步延迟），此时是空的
        r.sync()
        assert '（暂无提交）' in r.read()
        assert r.check()[0] == 0

        # 人为把清单改坏，再打 tag
        r.write(r.read().replace('（暂无提交）', '- tampered entry'))
        assert 'tampered entry' in r.read(), '篡改必须真的生效'
        r.git('add', '.')
        r.git('commit', '-qm', 'Freeze release notes')
        assert r.check()[0] == 1, '未打 tag 时仍应报漂移'

        r.git('tag', 'v1.1.0')
        rc, out, err = r.check()
        assert rc == 0, ('HEAD 在 tag 上时应跳过校验', rc, err)
        assert 'tag' in err, err
        before = r.read()
        rc, out, err = r.run()          # 刷新也必须跳过，不能改动已冻结内容
        assert rc == 0 and r.read() == before, '刷新不应改动已冻结的清单'
        assert 'tampered entry' in r.read()   # 仍是人为改坏的内容

        # --force 是发版后的逃生口：打 tag 的提交上重新基线清单
        rc, out, err = r.run('--force')
        assert rc == 0, (rc, err)
        assert 'tampered entry' not in r.read(), '--force 应重写清单'
        assert '（暂无提交）' in r.read()

        # tag 之后再提交一个提交，闸门重新生效（--force 重新基线后是自洽的）
        r.commit('Add post-release work', 'b.txt')
        assert r.check()[0] == 0, '重新基线后应自洽'
        r.commit('Add another post-release work', 'c.txt')
        assert r.check()[0] == 1, 'tag 之后必须恢复校验'
        r.sync()
        assert r.check()[0] == 0
    finally:
        r.close()


def case_crlf_file():
    """core.autocrlf 把工作区文件写成 CRLF 时，--check 不能误报漂移。

    这不是假设：Windows 上 Git 默认 core.autocrlf=true，一次 checkout 就会把
    CHANGELOG.md 重写成 CRLF。行尾差异与内容漂移必须分开。
    """
    r = Repo('gen-changelog-crlf-')
    try:
        r.init()
        with open(os.path.join(r.path, 'a.txt'), 'w',
                  encoding='utf-8', newline='') as handle:
            handle.write('x\n')
        r.write('# 更新日志\n')
        r.git('add', '.')
        r.git('commit', '-qm', 'Init')
        r.git('tag', 'v1.0.0')
        r.commit('Add widget module')
        r.sync()
        assert r.check()[0] == 0, 'LF 文件应通过'

        # 模拟 autocrlf：整个文件变 CRLF
        with open(r.changelog_path(), 'rb') as handle:
            crlf = handle.read().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        with open(r.changelog_path(), 'wb') as handle:
            handle.write(crlf)
        rc, out, err = r.check()
        assert rc == 0, ('CRLF 行尾不应被当成漂移', rc, err)

        # 刷新时必须跟随文件已有的行尾，不能把整个文件改回 LF
        r.commit('Add another widget', 'b.txt')
        r.sync()
        with open(r.changelog_path(), 'rb') as handle:
            after = handle.read()
        assert b'\r\n' in after, '应保留 CRLF'
        assert b'\n' not in after.replace(b'\r\n', b''), '不应出现裸 LF'
        assert r.check()[0] == 0, 'CRLF 文件刷新后仍应通过'
    finally:
        r.close()


def main():
    case_classification()
    case_idempotent_write()
    case_drift_gate()
    case_non_utf8()
    case_pr_merge_checkout()
    case_head_on_tag()
    case_crlf_file()
    print('gen_changelog self-test: ALL OK')


if __name__ == '__main__':
    main()