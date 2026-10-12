import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;
public class PrivScan extends GhidraScript {
  public void run() throws Exception {
    Set<String> m = new HashSet<>(Arrays.asList("IN","OUT","CLI","STI","WBINVD","INVD","RDMSR","WRMSR","HLT","LGDT","LIDT","INVLPG","INSB","INSW","INSD","OUTSB","OUTSW","OUTSD","RDPMC","CPUID","SIDT","SGDT"));
    PrintWriter w = new PrintWriter(new FileWriter(getScriptArgs()[0]));
    InstructionIterator it = currentProgram.getListing().getInstructions(true);
    while (it.hasNext()) { Instruction i = it.next(); String mn=i.getMnemonicString();
      String s=i.toString();
      if (m.contains(mn) || s.startsWith("MOV CR")||s.contains(",CR")||s.startsWith("MOV DR")||s.contains(",DR")) {
        Function f=getFunctionContaining(i.getAddress());
        w.println(i.getAddress()+"\t"+i.getLength()+"\t"+s+"\t"+(f==null?"":f.getName())); } }
    w.close();
  }
}
