// Exportiert alle Funktionen (Adresse + Groesse) und das komplette
// Dekompilat des geladenen Programms in ein Verzeichnis.
// Aufruf: ExportAll.java <outdir> [str:0xADDR ...]
//   str:0xADDR  -> listet alle Referenzen auf diese Adresse (inkl. Funktion)
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportAll extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outdir = args[0];
        new File(outdir).mkdirs();

        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.toggleSyntaxTree(true);
        di.setSimplificationStyle("decompile");
        if (!di.openProgram(currentProgram)) {
            println("EXPORTALL_FAIL openProgram");
            return;
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        PrintWriter fl = new PrintWriter(new FileWriter(outdir + "/functions.txt"));
        PrintWriter all = new PrintWriter(new FileWriter(outdir + "/ALL.c"));
        int n = 0, fail = 0;
        for (Function f : fm.getFunctions(true)) {
            fl.printf("0x%08x\t%d\t%s%n", f.getEntryPoint().getOffset(),
                      f.getBody().getNumAddresses(), f.getName());
            all.printf("/* FUNC %s @ 0x%08x size=%d */%n", f.getName(),
                       f.getEntryPoint().getOffset(), f.getBody().getNumAddresses());
            DecompileResults res = di.decompileFunction(f, 180, monitor);
            if (res.getDecompiledFunction() != null) {
                all.println(res.getDecompiledFunction().getC());
            } else {
                all.println("// decompile failed: " + res.getErrorMessage());
                fail++;
            }
            all.println();
            n++;
            if (n % 250 == 0) {
                println("progress " + n);
            }
        }
        fl.close();
        all.close();

        if (args.length > 1) {
            ReferenceManager rm = currentProgram.getReferenceManager();
            PrintWriter rx = new PrintWriter(new FileWriter(outdir + "/refs.txt"));
            for (int i = 1; i < args.length; i++) {
                String a = args[i];
                Address addr;
                if (a.startsWith("str:")) {
                    addr = toAddr(a.substring(4));
                } else {
                    addr = toAddr(a);
                }
                rx.println("## refs -> 0x" + addr);
                for (Reference r : rm.getReferencesTo(addr)) {
                    Address from = r.getFromAddress();
                    Function cf = fm.getFunctionContaining(from);
                    rx.printf("   from 0x%08x type=%s func=%s%n", from.getOffset(),
                              r.getReferenceType(),
                              cf == null ? "-" : cf.getName() + "@0x" + Long.toHexString(cf.getEntryPoint().getOffset()));
                }
                Function f = fm.getFunctionContaining(addr);
                rx.println("   containing func: " + (f == null ? "-" : f.getName() + "@0x" + Long.toHexString(f.getEntryPoint().getOffset())));
            }
            rx.close();
        }
        di.dispose();
        println("EXPORTALL_DONE funcs=" + n + " fail=" + fail + " out=" + outdir);
    }
}
