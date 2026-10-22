// Seeds functions from the Cortex-M vector table, re-runs analysis and writes
// every function's decompilation to the file given as the script argument.
// @category M32
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DumpDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address base = currentProgram.getMinAddress();
        for (int i = 1; i < 76; i++) {
            long v = getInt(base.add(4L * i)) & 0xffffffffL;
            if (v < 0x08003000L || v >= 0x08020000L) continue;
            Address a = toAddr(v & ~1L);
            disassemble(a);
            if (getFunctionAt(a) == null)
                new CreateFunctionCmd(a).applyTo(currentProgram);
            createLabel(a, i == 1 ? "Reset_Handler" : String.format("vec_%d", i), true);
        }
        analyzeAll(currentProgram);

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
            int n = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage() + "\n");
                n++;
            }
            println("decompiled " + n + " functions");
        }
    }
}
