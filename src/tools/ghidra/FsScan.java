import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
public class FsScan extends GhidraScript {
  public void run() throws Exception {
    PrintWriter w = new PrintWriter(new FileWriter(getScriptArgs()[0]));
    InstructionIterator it = currentProgram.getListing().getInstructions(true);
    while (it.hasNext()) { Instruction i = it.next(); String s = i.toString();
      if (s.contains("FS:")) { StringBuilder b=new StringBuilder(); for (byte x: i.getBytes()) b.append(String.format("%02x",x));
        w.println(i.getAddress()+"\t"+i.getLength()+"\t"+b+"\t"+s); } }
    w.close();
  }
}
