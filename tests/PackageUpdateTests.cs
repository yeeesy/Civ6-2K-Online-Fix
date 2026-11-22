using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

internal static class PackageUpdateTests {
    static void Check(bool ok,string text){Console.WriteLine((ok?"PASS: ":"FAIL: ")+text);if(!ok)throw new Exception(text);}
    static bool Reject(Action action){try{action();return false;}catch{return true;}}
    static void Write(string root,string path,string text){var p=Path.Combine(root,path.Replace('/',Path.DirectorySeparatorChar));Directory.CreateDirectory(Path.GetDirectoryName(p));File.WriteAllText(p,text,new UTF8Encoding(false));}
    static Dictionary<string,string> Hashes(string root){return Directory.GetFiles(root,"*",SearchOption.AllDirectories).ToDictionary(x=>x.Substring(root.Length+1).Replace('\\','/'),UpdateAgent.HashFile,StringComparer.OrdinalIgnoreCase);}
    static void Sums(string root){var files=Hashes(root);File.WriteAllLines(Path.Combine(root,PackageUpdate.Sums),files.Select(x=>x.Value+"  "+x.Key),new UTF8Encoding(false));}
    static bool Same(Dictionary<string,string> before,Dictionary<string,string> after){return before.Count==after.Count && before.All(x=>after.ContainsKey(x.Key) && after[x.Key]==x.Value);}
    internal static void Run(string temporary){
        var area=Path.Combine(temporary,"full");Directory.CreateDirectory(area);
        var source=Path.Combine(area,"incoming");Directory.CreateDirectory(source);
        Write(source,UpdateAgent.Exe,"new exe");Write(source,"README.md","new readme");
        Write(source,"source/src/new.h","new source");Write(source,"build_manifest.json","new build information");Sums(source);
        var package=new PackageUpdate.Package{Directory=source,Hashes=Hashes(source)};
        Func<string> fixture=delegate{
            var root=Path.Combine(area,Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
            Write(root,UpdateAgent.Exe,"old exe");Write(root,"README.md","old readme");
            Write(root,"source/src/obsolete.h","obsolete source");Write(root,"build_manifest.json","old build information");Sums(root);
            Write(root,"Civ6_2K_Online_Fix.ini","user configuration");Write(root,"logs/session.jsonl","user log");
            Write(root,"Feedback/report.txt","user feedback");Write(root,"my-notes.txt","personal notes");return root;
        };
        var target=fixture();var protectedFiles=new[]{"Civ6_2K_Online_Fix.ini","logs/session.jsonl","Feedback/report.txt","my-notes.txt"};
        var originals=protectedFiles.ToDictionary(x=>x,x=>UpdateAgent.HashFile(Path.Combine(target,x)));
        using(var tx=new PackageUpdate.Transaction(target,package,false)){
            tx.Install(null);Check(PackageUpdate.Installed(target,package),"full update installs all managed release files and its checksum manifest");tx.Commit();
        }
        Check(!File.Exists(Path.Combine(target,"source/src/obsolete.h")),"obsolete unmodified release source is removed");
        Check(originals.All(x=>UpdateAgent.HashFile(Path.Combine(target,x.Key))==x.Value),"user configuration logs feedback and additional files survive full update");
        foreach(var failAfter in new[]{1,3,5}){
            target=fixture();var before=Hashes(target);
            Check(Reject(()=>{using(var tx=new PackageUpdate.Transaction(target,package,false))tx.Install(n=>{if(n==failAfter)throw new IOException("injected write failure");});}) && Same(before,Hashes(target)),
                "failure after partial file installation restores the entire original directory");
        }
        target=fixture();var snapshot=Hashes(target);
        Check(Reject(()=>{using(var tx=new PackageUpdate.Transaction(target,package,false)){tx.Install(null);throw new Exception("self test failed");}}) && Same(snapshot,Hashes(target)),
              "post-install validation failure restores replaced deleted and newly added files");
        target=fixture();snapshot=Hashes(target);
        Check(Reject(()=>{using(var locked=File.Open(Path.Combine(target,"README.md"),FileMode.Open,FileAccess.Read,FileShare.None))
            using(var tx=new PackageUpdate.Transaction(target,package,false))tx.Install(null);}) && Same(snapshot,Hashes(target)),
              "locked managed file fails without losing earlier release files or user data");
        target=fixture();Write(target,"README.md","user edited managed file");snapshot=Hashes(target);
        Check(Reject(()=>{using(var tx=new PackageUpdate.Transaction(target,package,false))tx.Install(null);}) && Same(snapshot,Hashes(target)),
              "modified managed file is preserved and the update fails before replacement");
        target=fixture();snapshot=Hashes(target);FileStream lateLock=null;
        Check(Reject(()=>{try{using(var tx=new PackageUpdate.Transaction(target,package,false))
            tx.Install(n=>{if(n==1)lateLock=File.Open(Path.Combine(target,"build_manifest.json"),FileMode.Open,FileAccess.Read,FileShare.None);});}
            finally{if(lateLock!=null)lateLock.Dispose();}}) && Same(snapshot,Hashes(target)),
              "a sharing violation after the first replacement rolls back the complete file set");
        target=fixture();Write(target,"source/src/new.h","user owned collision");snapshot=Hashes(target);
        Check(Reject(()=>{using(var tx=new PackageUpdate.Transaction(target,package,false))tx.Install(null);}) && Same(snapshot,Hashes(target)),
              "an added release path cannot overwrite an unlisted user file");
        target=fixture();snapshot=Hashes(target);string pending;
        var interrupted=new PackageUpdate.Transaction(target,package,false);pending=interrupted.Work;
        try{interrupted.Install(n=>{if(n==3)throw new IOException("simulated interruption");});}catch(IOException){}
        PackageUpdate.Transaction.Recover(target,pending);
        Check(Same(snapshot,Hashes(target)),"persisted transaction journal can recover an interrupted partial installation");
        target=fixture();Write(target,UpdateAgent.Exe,"new exe");
        using(var tx=new PackageUpdate.Transaction(target,package,true)){tx.Install(null);tx.Commit();}
        Check(PackageUpdate.Installed(target,package),"legacy EXE-only handoff completes the remaining distribution files");
        target=fixture();Write(target,UpdateAgent.Exe,"new exe");snapshot=Hashes(target);
        Check(Reject(()=>{using(var tx=new PackageUpdate.Transaction(target,package,true)){tx.Install(null);tx.Commit();tx.RevertCommitted();throw new Exception("handoff failed");}}) &&
              Same(snapshot,Hashes(target)),"failed legacy handoff rolls back all companion files while its parent owns EXE rollback");
        Check(new[]{"../escape.txt","Civ6_2K_Online_Fix.ini","logs/session.jsonl","Feedback/report.txt","source/../secret.txt","source/NUL.txt","README.md:stream"}.All(x=>!PackageUpdate.Managed(x)),
              "update inventory rejects traversal user data reserved devices and alternate data streams");
        Check(Reject(()=>PackageUpdate.ReadSums(new string('A',64)+"  README.md\n"+new string('B',64)+"  readme.md\n")),
              "case-insensitive duplicate manifest paths fail closed");
    }
}
