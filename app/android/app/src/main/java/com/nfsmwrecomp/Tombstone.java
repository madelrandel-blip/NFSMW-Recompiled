package com.nfsmwrecomp;

import java.nio.charset.StandardCharsets;

/**
 * Reads the parts of a native crash's tombstone that say where it happened: the signal, the abort message and the
 * crashing thread's backtrace. The tombstone is a protobuf (AOSP system/core/debuggerd/proto/tombstone.proto); this
 * walks just the fields it needs, so no protobuf library is pulled in:
 *
 *   Tombstone: tid = 6, signal_info = 10 (Signal), abort_message = 14, causes = 15 (Cause), threads = 16 (map)
 *   Signal: name = 2, code_name = 4, fault_address = 9
 *   Cause: human_readable = 1
 *   map entry: key = 1 (tid), value = 2 (Thread)
 *   Thread: id = 1, name = 2, current_backtrace = 4 (BacktraceFrame)
 *   BacktraceFrame: rel_pc = 1, function_name = 4, function_offset = 5, file_name = 6
 *
 * Each frame prints its rel_pc, the offset inside its library, which addr2line on the unstripped libmain.so turns
 * into a source line.
 */
final class Tombstone {
    private Tombstone() {}

    /** A position in a protobuf message. */
    private static final class Reader {
        final byte[] data;
        int pos;
        final int end;

        Reader(byte[] data, int start, int end) {
            this.data = data;
            this.pos = start;
            this.end = end;
        }

        boolean more() {
            return pos < end;
        }

        long varint() {
            long value = 0;
            for (int shift = 0; shift < 64 && pos < end; shift += 7) {
                int b = data[pos++] & 0xFF;
                value |= (long) (b & 0x7F) << shift;
                if ((b & 0x80) == 0) {
                    break;
                }
            }
            return value;
        }

        /** Reads a length-delimited field and returns a reader over it. */
        Reader sub() {
            int length = (int) varint();
            Reader r = new Reader(data, pos, Math.min(pos + length, end));
            pos = r.end;
            return r;
        }

        String string() {
            Reader r = sub();
            return new String(data, r.pos, r.end - r.pos, StandardCharsets.UTF_8);
        }

        void skip(int wireType) {
            switch (wireType) {
                case 0: varint(); break;
                case 1: pos += 8; break;
                case 2: sub(); break;
                case 5: pos += 4; break;
                default: pos = end; break;  // groups are not used by the tombstone
            }
        }
    }

    static void append(byte[] data, StringBuilder out, int maxFrames) {
        Reader top = new Reader(data, 0, data.length);
        long crashTid = -1;
        String signal = null;
        String abort = null;
        StringBuilder causes = new StringBuilder();
        // The threads are kept as ranges until the whole message is read: the crashing tid may come after them.
        java.util.List<Reader> threads = new java.util.ArrayList<>();
        while (top.more()) {
            long key = top.varint();
            int field = (int) (key >>> 3);
            int wire = (int) (key & 7);
            if (field == 6 && wire == 0) {
                crashTid = top.varint();
            } else if (field == 10 && wire == 2) {
                signal = signal(top.sub());
            } else if (field == 14 && wire == 2) {
                abort = top.string();
            } else if (field == 15 && wire == 2) {
                Reader cause = top.sub();
                while (cause.more()) {
                    long k = cause.varint();
                    if ((k >>> 3) == 1 && (k & 7) == 2) {
                        causes.append("cause: ").append(cause.string()).append('\n');
                    } else {
                        cause.skip((int) (k & 7));
                    }
                }
            } else if (field == 16 && wire == 2) {
                threads.add(top.sub());
            } else {
                top.skip(wire);
            }
        }
        if (signal != null) {
            out.append(signal).append('\n');
        }
        if (abort != null && !abort.isEmpty()) {
            out.append("abort message: ").append(abort).append('\n');
        }
        out.append(causes);
        for (Reader entry : threads) {
            long tid = -1;
            Reader thread = null;
            while (entry.more()) {
                long k = entry.varint();
                if ((k >>> 3) == 1 && (k & 7) == 0) {
                    tid = entry.varint();
                } else if ((k >>> 3) == 2 && (k & 7) == 2) {
                    thread = entry.sub();
                } else {
                    entry.skip((int) (k & 7));
                }
            }
            if (tid == crashTid && thread != null) {
                thread(thread, out, maxFrames);
                return;
            }
        }
        out.append("(crashing thread ").append(crashTid).append(" not found)\n");
    }

    private static String signal(Reader r) {
        String name = "";
        String code = "";
        long fault = 0;
        while (r.more()) {
            long k = r.varint();
            int field = (int) (k >>> 3);
            int wire = (int) (k & 7);
            if (field == 2 && wire == 2) {
                name = r.string();
            } else if (field == 4 && wire == 2) {
                code = r.string();
            } else if (field == 9 && wire == 0) {
                fault = r.varint();
            } else {
                r.skip(wire);
            }
        }
        return "signal " + name + " (" + code + "), fault address 0x" + Long.toHexString(fault);
    }

    private static void thread(Reader r, StringBuilder out, int maxFrames) {
        int frame = 0;
        while (r.more()) {
            long k = r.varint();
            int field = (int) (k >>> 3);
            int wire = (int) (k & 7);
            if (field == 2 && wire == 2) {
                out.append("thread: ").append(r.string()).append('\n');
            } else if (field == 4 && wire == 2) {
                Reader f = r.sub();
                if (frame >= maxFrames) {
                    continue;
                }
                long relPc = 0;
                long offset = 0;
                String function = "";
                String file = "";
                while (f.more()) {
                    long fk = f.varint();
                    int ff = (int) (fk >>> 3);
                    int fw = (int) (fk & 7);
                    if (ff == 1 && fw == 0) {
                        relPc = f.varint();
                    } else if (ff == 4 && fw == 2) {
                        function = f.string();
                    } else if (ff == 5 && fw == 0) {
                        offset = f.varint();
                    } else if (ff == 6 && fw == 2) {
                        file = f.string();
                    } else {
                        f.skip(fw);
                    }
                }
                int slash = file.lastIndexOf('/');
                out.append(String.format("#%02d pc %08x %s", frame, relPc, slash >= 0 ? file.substring(slash + 1) : file));
                if (!function.isEmpty()) {
                    out.append(" (").append(function).append('+').append(offset).append(')');
                }
                out.append('\n');
                ++frame;
            } else {
                r.skip(wire);
            }
        }
    }
}
