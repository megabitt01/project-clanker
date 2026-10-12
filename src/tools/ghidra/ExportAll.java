import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
public class ExportAll extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    new File(out+"/decomp").mkdirs();
    PrintWriter fl = new PrintWriter(new FileWriter(out+"/functions.tsv"));
    FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
    java.util.List<Function> fs = new java.util.ArrayList<>();
    while (it.hasNext()) { Function f = it.next(); fs.add(f);
      fl.println(f.getEntryPoint()+"\t"+f.getName()+"\t"+f.getBody().getNumAddresses()+"\t"+f.getCallingConventionName()+"\t"+f.getSignature().getPrototypeString()); }
    fl.close();
    PrintWriter sl = new PrintWriter(new FileWriter(out+"/symbols.tsv"));
    SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
    while (si.hasNext()) { Symbol s = si.next(); if (s.getSource()!=SourceType.DEFAULT) sl.println(s.getAddress()+"\t"+s.getName(true)+"\t"+s.getSymbolType()); }
    sl.close();
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    int n=0; PrintWriter cw=null; int chunk=-1;
    for (Function f : fs) {
      if (monitor.isCancelled()) break;
      int c = (int)(f.getEntryPoint().getOffset() >> 16);
      if (c!=chunk) { if (cw!=null) cw.close(); chunk=c; cw=new PrintWriter(new FileWriter(out+"/decomp/"+String.format("%04x",c)+".c")); }
      DecompileResults r = di.decompileFunction(f, 60, monitor);
      cw.println("// ==== "+f.getEntryPoint()+" "+f.getName());
      if (r!=null && r.decompileCompleted()) cw.println(r.getDecompiledFunction().getC()); else cw.println("// decompile failed");
      if (++n % 500 == 0) println("decompiled "+n+"/"+fs.size());
    }
    if (cw!=null) cw.close();
    println("DONE "+n);
  }
}
