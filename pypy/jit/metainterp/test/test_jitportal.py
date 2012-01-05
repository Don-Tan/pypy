
from pypy.rlib.jit import JitDriver, JitPortal
from pypy.jit.metainterp.test.support import LLJitMixin
from pypy.jit.codewriter.policy import JitPolicy
from pypy.jit.metainterp.jitprof import ABORT_FORCE_QUASIIMMUT

class TestJitPortal(LLJitMixin):
    def test_abort_quasi_immut(self):
        reasons = []
        
        class MyJitPortal(JitPortal):
            def on_abort(self, reason, jitdriver, greenkey):
                assert jitdriver is myjitdriver
                assert len(greenkey) == 1
                reasons.append(reason)

        portal = MyJitPortal()

        myjitdriver = JitDriver(greens=['foo'], reds=['x', 'total'])

        class Foo:
            _immutable_fields_ = ['a?']
            def __init__(self, a):
                self.a = a
        def f(a, x):
            foo = Foo(a)
            total = 0
            while x > 0:
                myjitdriver.jit_merge_point(foo=foo, x=x, total=total)
                # read a quasi-immutable field out of a Constant
                total += foo.a
                foo.a += 1
                x -= 1
            return total
        #
        assert f(100, 7) == 721
        res = self.meta_interp(f, [100, 7], policy=JitPolicy(portal))
        assert res == 721
        assert reasons == [ABORT_FORCE_QUASIIMMUT] * 2

    def test_on_compile(self):
        called = {}
        
        class MyJitPortal(JitPortal):
            def on_compile(self, jitdriver, logger, looptoken, operations,
                           type, greenkey, asmaddr, asmlen):
                assert asmaddr == 0
                assert asmlen == 0
                called[(greenkey[1].getint(), greenkey[0].getint(), type)] = looptoken

        portal = MyJitPortal()

        driver = JitDriver(greens = ['n', 'm'], reds = ['i'])

        def loop(n, m):
            i = 0
            while i < n + m:
                driver.can_enter_jit(n=n, m=m, i=i)
                driver.jit_merge_point(n=n, m=m, i=i)
                i += 1

        self.meta_interp(loop, [1, 4], policy=JitPolicy(portal))
        assert sorted(called.keys()) == [(4, 1, "loop")]
        self.meta_interp(loop, [2, 4], policy=JitPolicy(portal))
        assert sorted(called.keys()) == [(4, 1, "loop"),
                                         (4, 2, "loop")]

    def test_on_compile_bridge(self):
        called = {}
        
        class MyJitPortal(JitPortal):
            def on_compile(self, jitdriver, logger, looptoken, operations,
                           type, greenkey, asmaddr, asmlen):
                assert asmaddr == 0
                assert asmlen == 0
                called[(greenkey[1].getint(), greenkey[0].getint(), type)] = looptoken

            def on_compile_bridge(self, jitdriver, logger, orig_token,
                                  operations, n, asmstart, asmlen):
                assert 'bridge' not in called
                called['bridge'] = orig_token

        driver = JitDriver(greens = ['n', 'm'], reds = ['i'])

        def loop(n, m):
            i = 0
            while i < n + m:
                driver.can_enter_jit(n=n, m=m, i=i)
                driver.jit_merge_point(n=n, m=m, i=i)
                if i >= 4:
                    i += 2
                i += 1

        self.meta_interp(loop, [1, 10], policy=JitPolicy(MyJitPortal()))
        assert sorted(called.keys()) == ['bridge', (10, 1, "loop")]

