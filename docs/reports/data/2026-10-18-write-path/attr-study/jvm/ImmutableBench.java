// JVM counterpart of protoCore's attribute_lookup_benchmark (own/sweep/upd).
// Same shapes: nattr 4 and 16, value(i, a) = i + a, read attribute nattr/2,
// update attribute 0, uniform random object order, best of 5 timed reps after
// 3 warm-up reps, checksum verified on every rep.
//
// Structures:
//   record  - Java record with a copy ("wither"): the bytecode of a Scala
//             case class and its copy(); a fixed shape resolved at compile time.
//   clj     - Clojure persistent map with keyword keys (PersistentArrayMap up
//             to 8 entries, PersistentHashMap above): dynamic keys, immutable.
//   scala   - scala.collection.immutable.HashMap (CHAMP) with String keys.
//   mutable - a plain Java class with mutable fields, written in place: the
//             JVM's own object model, for reference.
import clojure.lang.IPersistentMap;
import clojure.lang.Keyword;
import clojure.lang.PersistentArrayMap;
import clojure.lang.PersistentHashMap;
import java.util.Random;

public class ImmutableBench {
    static final long OPS = 4_000_000;
    static final long UOPS = 500_000;
    static final int WARM = 3, REPS = 5;

    record R4(long a0, long a1, long a2, long a3) {
        R4 withA0(long v) { return new R4(v, a1, a2, a3); }
    }
    record R16(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7,
               long a8, long a9, long a10, long a11, long a12, long a13, long a14, long a15) {
        R16 withA0(long v) { return new R16(v, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15); }
    }

    static final class M4 { long a0, a1, a2, a3; M4(long i) { a0 = i; a1 = i + 1; a2 = i + 2; a3 = i + 3; } }
    static final class M16 {
        long a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15;
        M16(long i) { a0 = i; a1 = i + 1; a2 = i + 2; a3 = i + 3; a4 = i + 4; a5 = i + 5; a6 = i + 6; a7 = i + 7;
                      a8 = i + 8; a9 = i + 9; a10 = i + 10; a11 = i + 11; a12 = i + 12; a13 = i + 13; a14 = i + 14; a15 = i + 15; }
    }

    interface Body { long run(); }

    static int[] order(int nobj, long ops) {
        Random rng = new Random(42);
        int[] o = new int[(int) ops];
        for (int i = 0; i < ops; i++) o[i] = rng.nextInt(nobj);
        return o;
    }

    static void time(String label, int nattr, long ops, long expected, Body b) {
        double best = 1e30;
        for (int r = 0; r < WARM + REPS; r++) {
            long t0 = System.nanoTime();
            long s = b.run();
            double ns = (double) (System.nanoTime() - t0) / ops;
            if (s != expected) throw new IllegalStateException("CHECKSUM MISMATCH " + label + " " + s + " != " + expected);
            if (r >= WARM) best = Math.min(best, ns);
        }
        System.out.printf("%-34s %4d  %8.2f ns/op  (ops=%d, checksum ok)%n", label, nattr, best, ops);
    }

    static Object[] build(String kind, int nattr, int nobj, Keyword[] kw, String[] sk) {
        Object[] objs = new Object[nobj];
        for (int i = 0; i < nobj; i++) {
            switch (kind) {
                case "record" -> objs[i] = nattr == 4 ? new R4(i, i + 1, i + 2, i + 3)
                        : new R16(i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7, i + 8, i + 9, i + 10, i + 11, i + 12, i + 13, i + 14, i + 15);
                case "mutable" -> objs[i] = nattr == 4 ? new M4(i) : new M16(i);
                case "clj" -> {
                    IPersistentMap m = nattr <= 8 ? PersistentArrayMap.EMPTY : PersistentHashMap.EMPTY;
                    for (int a = 0; a < nattr; a++) m = m.assoc(kw[a], (long) (i + a));
                    objs[i] = m;
                }
                case "scala" -> {
                    scala.collection.immutable.HashMap<String, Object> m = scala.collection.immutable.HashMap$.MODULE$.empty();
                    for (int a = 0; a < nattr; a++) m = m.updated(sk[a], (Object) (long) (i + a));
                    objs[i] = m;
                }
            }
        }
        return objs;
    }

    // One specialised loop per structure and shape: no dispatch inside the timed loop.
    static Body readLoop(String kind, int nattr, Object[] objs, int[] ord, Keyword k, String sk) {
        if (kind.equals("record") && nattr == 4) return () -> { long s = 0; for (int i = 0; i < ord.length; i++) s += ((R4) objs[ord[i]]).a2(); return s; };
        if (kind.equals("record")) return () -> { long s = 0; for (int i = 0; i < ord.length; i++) s += ((R16) objs[ord[i]]).a8(); return s; };
        if (kind.equals("mutable") && nattr == 4) return () -> { long s = 0; for (int i = 0; i < ord.length; i++) s += ((M4) objs[ord[i]]).a2; return s; };
        if (kind.equals("mutable")) return () -> { long s = 0; for (int i = 0; i < ord.length; i++) s += ((M16) objs[ord[i]]).a8; return s; };
        if (kind.equals("clj")) return () -> { long s = 0; for (int i = 0; i < ord.length; i++) s += (Long) ((IPersistentMap) objs[ord[i]]).valAt(k); return s; };
        return () -> { long s = 0; for (int i = 0; i < ord.length; i++) s += (Long) ((scala.collection.immutable.HashMap<String, Object>) objs[ord[i]]).apply(sk); return s; };
    }

    @SuppressWarnings("unchecked")
    static Body updateLoop(String kind, int nattr, Object[] objs, int[] ord, Keyword wk, String ws, Keyword rk, String rs) {
        if (kind.equals("record") && nattr == 4) return () -> {
            Object[] cur = objs.clone(); long s = 0;
            for (int i = 0; i < ord.length; i++) { int x = ord[i]; R4 n = ((R4) cur[x]).withA0(i & 1023); cur[x] = n; s += n.a2(); }
            return s; };
        if (kind.equals("record")) return () -> {
            Object[] cur = objs.clone(); long s = 0;
            for (int i = 0; i < ord.length; i++) { int x = ord[i]; R16 n = ((R16) cur[x]).withA0(i & 1023); cur[x] = n; s += n.a8(); }
            return s; };
        if (kind.equals("mutable") && nattr == 4) return () -> {
            long s = 0;
            for (int i = 0; i < ord.length; i++) { M4 o = (M4) objs[ord[i]]; o.a0 = i & 1023; s += o.a2; }
            return s; };
        if (kind.equals("mutable")) return () -> {
            long s = 0;
            for (int i = 0; i < ord.length; i++) { M16 o = (M16) objs[ord[i]]; o.a0 = i & 1023; s += o.a8; }
            return s; };
        if (kind.equals("clj")) return () -> {
            Object[] cur = objs.clone(); long s = 0;
            for (int i = 0; i < ord.length; i++) { int x = ord[i]; IPersistentMap n = ((IPersistentMap) cur[x]).assoc(wk, (long) (i & 1023)); cur[x] = n; s += (Long) n.valAt(rk); }
            return s; };
        return () -> {
            Object[] cur = objs.clone(); long s = 0;
            for (int i = 0; i < ord.length; i++) { int x = ord[i];
                scala.collection.immutable.HashMap<String, Object> n = ((scala.collection.immutable.HashMap<String, Object>) cur[x]).updated(ws, (Object) (long) (i & 1023));
                cur[x] = n; s += (Long) n.apply(rs); }
            return s; };
    }

    public static void main(String[] args) {
        String mode = args.length > 0 ? args[0] : "all";
        Keyword[] kw = new Keyword[16];
        String[] sk = new String[16];
        for (int a = 0; a < 16; a++) { kw[a] = Keyword.intern("attr_" + a); sk[a] = ("attr_" + a).intern(); }
        System.out.printf("# java %s%n", System.getProperty("java.version"));
        for (int nobj : new int[]{1, 4096, 16384, 65536}) {
            System.out.printf("-- nobj=%d%n", nobj);
            for (String kind : new String[]{"mutable", "record", "clj", "scala"}) {
                for (int nattr : new int[]{4, 16}) {
                    final Object[] objs = build(kind, nattr, nobj, kw, sk);
                    final int r = nattr / 2;
                    final Keyword rk = kw[r], wk = kw[0];
                    final String rs = sk[r], ws = sk[0];
                    if (mode.equals("all") || mode.equals("read")) {
                        final int[] ord = order(nobj, OPS);
                        long exp = 0;
                        for (int i = 0; i < OPS; i++) exp += ord[i] + r;
                        time(kind + "/read nattr", nattr, OPS, exp, readLoop(kind, nattr, objs, ord, rk, rs));
                    }
                    if (mode.equals("all") || mode.equals("upd")) {
                        final int[] ord = order(nobj, UOPS);
                        long exp = 0;
                        for (int i = 0; i < UOPS; i++) exp += ord[i] + r;
                        time(kind + "/update copy nattr", nattr, UOPS, exp, updateLoop(kind, nattr, objs, ord, wk, ws, rk, rs));
                    }
                }
            }
        }
    }
}
