import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.io.*;
public class Dis extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs(); PrintWriter w = new PrintWriter(new FileWriter(a[0]));
    for (int k=1;k+1<a.length;k+=2) {
      Address s = toAddr(Long.parseLong(a[k],16)); int n = Integer.parseInt(a[k+1]);
      new DisassembleCommand(s,null,true).applyTo(currentProgram, monitor);
      Instruction i = getInstructionAt(s);
      w.println("---- "+a[k]);
      for (int j=0;j<n && i!=null;j++){ w.println(i.getAddress()+"  "+i); i=i.getNext(); }
    }
    w.close();
  }
}
