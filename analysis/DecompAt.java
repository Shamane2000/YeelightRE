// Define (if missing) and decompile the function at a given address, write C to file.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;

public class DecompAt extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address a = toAddr(args[0]);
        String out = args.length > 1 ? args[1] : "/tmp/decomp_at.txt";
        StringBuilder log = new StringBuilder();
        Function f = getFunctionAt(a);
        Function cont = getFunctionContaining(a);
        log.append("addr=").append(a).append("\n");
        log.append("functionAt=").append(f == null ? "null" : f.getName() + " body=" + f.getBody().getNumAddresses()).append("\n");
        log.append("functionContaining=").append(cont == null ? "null" : cont.getName() + " entry=" + cont.getEntryPoint() + " body=" + cont.getBody().getNumAddresses()).append("\n");
        if (f == null) {
            f = createFunction(a, "ota_worker_task_1f008784");
            log.append("createFunction=").append(f == null ? "FAILED" : "OK " + f.getName() + " body=" + f.getBody().getNumAddresses()).append("\n");
        }
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        pw.println("// " + log.toString().replace("\n", "\n// "));
        if (f == null) {
            pw.println("// NO FUNCTION");
            pw.close();
            println("DECOMPAT_FAIL " + out);
            return;
        }
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.toggleSyntaxTree(true);
        di.setSimplificationStyle("decompile");
        if (!di.openProgram(currentProgram)) {
            pw.println("// openProgram FAILED");
            pw.close();
            println("DECOMPAT_FAIL openProgram");
            return;
        }
        DecompileResults res = di.decompileFunction(f, 180, monitor);
        String c = (res.getDecompiledFunction() != null) ? res.getDecompiledFunction().getC()
                : "// no C: " + res.getErrorMessage();
        pw.println("/* FUNCTION " + f.getName() + " @ 0x" + Long.toHexString(f.getEntryPoint().getOffset())
                + " size=" + f.getBody().getNumAddresses() + " */");
        pw.println(c);
        pw.close();
        di.dispose();
        println("DECOMPAT_DONE " + out + " len=" + c.length());
    }
}
