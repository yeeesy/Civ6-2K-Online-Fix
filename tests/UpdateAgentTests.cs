using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

internal static class UpdateAgentTests {
    static void Check(bool ok,string text){if(!ok)throw new Exception(text); Console.WriteLine("PASS: "+text);}
    static bool Reject(Action f){try{f();return false;}catch{return true;}}
    static void Put(ZipArchive z,string name,string s){using(var w=new StreamWriter(z.CreateEntry(name).Open(),new UTF8Encoding(false)))w.Write(s);}
    public static int Main(){
        var t=Path.Combine(Path.GetTempPath(),"Civ6Fix-Update-Test-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(t);
        try{
            const string v="9.10.0", content="synthetic payload"; var entry="Civ6_2K_Online_Fix_"+v+"/"+UpdateAgent.Exe;
            Check(UpdateAgent.Newer(v,"9.9.0") && UpdateAgent.Newer(v,"9.10.0-rc.1") &&
                  !UpdateAgent.Newer(v,v) && !UpdateAgent.Newer("1.0.0","2.0.0") && !UpdateAgent.Stable("../9.10.0"),
                  "update comparison handles numeric versions, preview promotion, and rollback");
            var zip=Path.Combine(t,"test.zip");var sample=Path.Combine(t,"expected");
            File.WriteAllText(sample,content,new UTF8Encoding(false));
            using(var f=File.Create(zip))using(var z=new ZipArchive(f,ZipArchiveMode.Create)){
                Put(z,"../escape.txt","ignored");Put(z,entry,content);
            }
            var body=Encoding.UTF8.GetBytes(String.Join("\n",new[]{"CIV6-UPDATE/1",UpdateAgent.Repo,v,
                new FileInfo(zip).Length.ToString(),UpdateAgent.HashFile(zip),new FileInfo(sample).Length.ToString(),UpdateAgent.HashFile(sample)}));
            using(var rsa=new RSACryptoServiceProvider(3072,new CspParameters(24){Flags=CspProviderFlags.CreateEphemeralKey})){
                rsa.PersistKeyInCsp=false;
                var sig=rsa.SignData(body,CryptoConfig.MapNameToOID("SHA256"));
                var envelope=Convert.ToBase64String(body)+"\n"+Convert.ToBase64String(sig);
                var m=UpdateAgent.Verify(envelope,v,rsa.ToXmlString(false));
                Check(Reject(()=>UpdateAgent.Verify(envelope,"9.10.1",rsa.ToXmlString(false))),"release metadata cannot be reused for another version");
                sig[2]^=0x20;
                Check(Reject(()=>UpdateAgent.Verify(Convert.ToBase64String(body)+"\n"+Convert.ToBase64String(sig),v,rsa.ToXmlString(false))),
                      "invalid update signatures are rejected");
                var outDir=Directory.CreateDirectory(Path.Combine(t,"stage")).FullName;
                var outFile=Path.Combine(outDir,"payload");
                UpdateAgent.Extract(zip,outFile,m);
                Check(File.ReadAllText(outFile)==content && !File.Exists(Path.Combine(t,"escape.txt")),"only selected verified bytes are extracted");
                var duplicate=Path.Combine(t,"duplicate.zip");
                using(var f=File.Create(duplicate))using(var z=new ZipArchive(f,ZipArchiveMode.Create)){Put(z,entry,content);Put(z,entry,content);}
                Check(Reject(()=>UpdateAgent.Extract(duplicate,Path.Combine(t,"bad-archive"),m)),"a changed archive fails its signed hash or length");
                m.ZipHash=UpdateAgent.HashFile(duplicate);m.ZipSize=new FileInfo(duplicate).Length;
                Check(Reject(()=>UpdateAgent.Extract(duplicate,Path.Combine(t,"ambiguous"),m)),"duplicate executable entries fail closed");
            }
            var target=Path.Combine(t,"target");var incoming=Path.Combine(t,"incoming");var bak=Path.Combine(t,"backup");
            File.WriteAllText(target,"old");File.WriteAllText(incoming,"new");var old=UpdateAgent.HashFile(target);var next=UpdateAgent.HashFile(incoming);
            Check(Reject(()=>UpdateAgent.Swap(target,incoming,bak,old,next,delegate{throw new Exception("synthetic start failure");})) &&
                  UpdateAgent.HashFile(target)==old,"failed post-install validation restores the previous launcher");
            File.WriteAllText(incoming,"new");
            UpdateAgent.Swap(target,incoming,bak,old,next,delegate{});
            Check(UpdateAgent.HashFile(target)==next && UpdateAgent.HashFile(bak)==old,"successful update swaps only the target and retains matching backup");
            return 0;
        } finally {foreach(var file in Directory.GetFiles(t,"*",SearchOption.AllDirectories)) File.Delete(file);
            var child=Path.Combine(t,"stage");if(Directory.Exists(child))Directory.Delete(child);Directory.Delete(t);}
    }
}
