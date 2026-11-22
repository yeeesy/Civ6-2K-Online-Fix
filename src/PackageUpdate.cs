using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

// All paths come from an authenticated archive, then pass a separate filesystem
// policy. Configuration, feedback and user logs never enter a transaction.
internal static class PackageUpdate {
    internal const string Sums = "SHA256SUMS.txt";
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false,true);
    static readonly string[] Roots = { UpdateAgent.Exe,"build_manifest.json","README.md","CHANGELOG.md",
        "LICENSE","SECURITY.md","Civ6_2K_Online_Fix.ini.example","verify_hashes.ps1",Sums };
    static void Need(bool ok) { if(!ok)throw new InvalidDataException("package update validation failed"); }
    internal static bool Hex(string value) { return value!=null && Regex.IsMatch(value,@"\A[0-9A-Fa-f]{64}\z"); }
    internal static bool Managed(string path) {
        if(String.IsNullOrEmpty(path) || path.Length>200 || path.Contains('\\') || path.Contains(':') || path.StartsWith("/"))return false;
        foreach(var part in path.Split('/')){
            if(!Regex.IsMatch(part,@"\A[A-Za-z0-9_.-]+\z") || part=="." || part==".." || part.EndsWith(".") ||
               Regex.IsMatch(part,@"\A(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)",RegexOptions.IgnoreCase))return false;
        }
        if(Roots.Contains(path,StringComparer.OrdinalIgnoreCase) || path=="logs/README.txt")return true;
        if(path.StartsWith("docs/",StringComparison.Ordinal))return path.EndsWith(".md",StringComparison.OrdinalIgnoreCase);
        if(!path.StartsWith("source/",StringComparison.Ordinal))return false;
        var name=Path.GetFileName(path);
        if(name=="LICENSE" || name==".editorconfig" || name==".gitignore")return true;
        return new[]{".cpp",".h",".cs",".xml",".rc",".in",".ps1",".py",".md",".txt",".json",".yml",".yaml",".ini.example"}
            .Any(s=>name.EndsWith(s,StringComparison.OrdinalIgnoreCase));
    }
    internal static void Plain(string path) {
        for(var p=Path.GetFullPath(path);!String.IsNullOrEmpty(p);p=Path.GetDirectoryName(p)){
            if(File.Exists(p) || Directory.Exists(p))Need((File.GetAttributes(p)&FileAttributes.ReparsePoint)==0);
        }
    }
    internal static string At(string root,string relative) {
        Need(Managed(relative));
        var parent=Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar;
        var path=Path.GetFullPath(Path.Combine(parent,relative.Replace('/',Path.DirectorySeparatorChar)));
        Need(path.StartsWith(parent,StringComparison.OrdinalIgnoreCase));Plain(path);return path;
    }
    internal static Dictionary<string,string> ReadSums(string text) {
        Need(text.Length<=1024*1024);
        var result=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        foreach(var raw in text.Replace("\r\n","\n").Split('\n')){
            if(raw.Length==0)continue;
            Need(raw.Length>66 && raw.Substring(64,2)=="  ");
            var hash=raw.Substring(0,64);var path=raw.Substring(66);
            Need(Hex(hash) && Managed(path) && path!=Sums && !result.ContainsKey(path));
            result.Add(path,hash.ToUpperInvariant());Need(result.Count<=4096);
        }
        Need(result.ContainsKey(UpdateAgent.Exe));return result;
    }
    internal sealed class Package {
        internal string Directory;
        internal Dictionary<string,string> Hashes;
    }
    internal static Package StageZip(string zip,string destination,UpdateAgent.Manifest manifest) {
        Plain(zip);Plain(destination);
        Need(!Directory.Exists(destination) && !File.Exists(destination));
        using(var file=File.OpenRead(zip))Need(file.Length==manifest.ZipSize && UpdateAgent.HashFile(zip)==manifest.ZipHash);
        var prefix="Civ6_2K_Online_Fix_"+manifest.Version+"/";
        using(var file=File.OpenRead(zip))using(var archive=new ZipArchive(file,ZipArchiveMode.Read)){
            Need(archive.Entries.Count<=8192);
            var entries=new Dictionary<string,ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);long total=0;
            foreach(var entry in archive.Entries){
                Need(entry.FullName.StartsWith(prefix,StringComparison.Ordinal) && !entry.FullName.Contains('\\'));
                var relative=entry.FullName.Substring(prefix.Length);
                if(relative=="" && entry.Length==0)continue;
                if(relative.EndsWith("/",StringComparison.Ordinal)){
                    Need(entry.Length==0 && relative.Split('/').All(x=>x=="" || Regex.IsMatch(x,@"\A[A-Za-z0-9_.-]+\z") && x!=".." && x!="."));continue;
                }
                Need(Managed(relative) && !entries.ContainsKey(relative) && entry.Length<=16*1024*1024);
                total+=entry.Length;Need(total<=128*1024*1024 && entries.Count<4096);
                entries.Add(relative,entry);
            }
            Need(entries.ContainsKey(Sums) && entries[Sums].Length<=1024*1024);
            string sums;using(var reader=new StreamReader(entries[Sums].Open(),Utf8,false))sums=reader.ReadToEnd();
            var hashes=ReadSums(sums);
            Need(entries.Count==hashes.Count+1 && hashes.All(x=>entries.ContainsKey(x.Key)) &&
                 hashes[UpdateAgent.Exe]==manifest.ExeHash && entries[UpdateAgent.Exe].Length==manifest.ExeSize);
            System.IO.Directory.CreateDirectory(destination);
            foreach(var pair in entries){
                var path=At(destination,pair.Key);System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path));
                using(var input=pair.Value.Open())using(var output=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None)){
                    var buffer=new byte[65536];long copied=0;int n;
                    while((n=input.Read(buffer,0,buffer.Length))!=0){copied+=n;Need(copied<=pair.Value.Length);output.Write(buffer,0,n);}
                    Need(copied==pair.Value.Length);output.Flush(true);
                }
                if(pair.Key!=Sums)Need(String.Equals(UpdateAgent.HashFile(path),hashes[pair.Key],StringComparison.OrdinalIgnoreCase));
            }
            hashes.Add(Sums,UpdateAgent.HashFile(At(destination,Sums)));
            return new Package{Directory=destination,Hashes=hashes};
        }
    }
    internal static bool Installed(string root,Package package) {
        return package.Hashes.All(x=>File.Exists(At(root,x.Key)) &&
            String.Equals(UpdateAgent.HashFile(At(root,x.Key)),x.Value,StringComparison.OrdinalIgnoreCase));
    }
    public sealed class Change {
        public string Path,OldHash,NewHash;
        public bool Existed;
    }
    public sealed class Journal {
        public string State;
        public List<Change> Changes;
        public List<string> Directories;
    }
    internal sealed class Transaction : IDisposable {
        readonly string root,work;
        readonly Journal journal;
        bool committed,rolledBack;
        internal string Work {get{return work;}}
        internal Transaction(string targetRoot,Package package,bool legacy) {
            root=Path.GetFullPath(targetRoot);Plain(root);
            Need(Directory.Exists(root) && root!=Path.GetPathRoot(root) && !root.StartsWith(@"\\"));
            var oldFile=At(root,Sums);
            var old=File.Exists(oldFile)?ReadSums(File.ReadAllText(oldFile,Utf8)):
                new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            var plan=new List<Change>();
            foreach(var pair in package.Hashes.OrderBy(x=>x.Key==UpdateAgent.Exe?2:x.Key==Sums?1:0).ThenBy(x=>x.Key,StringComparer.Ordinal)){
                if(legacy && String.Equals(pair.Key,UpdateAgent.Exe,StringComparison.OrdinalIgnoreCase))continue;
                var target=At(root,pair.Key);Need(!Directory.Exists(target));var exists=File.Exists(target);
                var previous=exists?UpdateAgent.HashFile(target):null;
                if(exists && pair.Key!=Sums && !String.Equals(pair.Key,UpdateAgent.Exe,StringComparison.OrdinalIgnoreCase)){
                    string expected;Need(old.TryGetValue(pair.Key,out expected) && String.Equals(expected,previous,StringComparison.OrdinalIgnoreCase));
                }
                plan.Add(new Change{Path=pair.Key,OldHash=previous,NewHash=pair.Value,Existed=exists});
            }
            foreach(var pair in old.Where(x=>!package.Hashes.ContainsKey(x.Key))){
                var target=At(root,pair.Key);Need(!Directory.Exists(target));
                if(File.Exists(target)){
                    Need(String.Equals(UpdateAgent.HashFile(target),pair.Value,StringComparison.OrdinalIgnoreCase));
                    plan.Add(new Change{Path=pair.Key,OldHash=pair.Value,Existed=true});
                }
            }
            work=Path.Combine(root,".civ6-update-"+Guid.NewGuid().ToString("N"));
            Need(!Directory.Exists(work));Directory.CreateDirectory(work);
            journal=new Journal{State="prepared",Changes=plan,Directories=new List<string>()};
            try{
                foreach(var change in plan.Where(x=>x.NewHash!=null)){
                    var incoming=At(Path.Combine(work,"new"),change.Path);Directory.CreateDirectory(Path.GetDirectoryName(incoming));
                    File.Copy(At(package.Directory,change.Path),incoming,false);Need(UpdateAgent.HashFile(incoming)==change.NewHash);
                }
                Save();
            }catch{Cleanup();throw;}
        }
        Transaction(string targetRoot,string existing,Journal state) {root=targetRoot;work=existing;journal=state;}
        void Save() {
            var text=new JavaScriptSerializer().Serialize(journal);
            var temporary=Path.Combine(work,"journal.next");var destination=Path.Combine(work,"journal.json");
            using(var f=new FileStream(temporary,FileMode.Create,FileAccess.Write,FileShare.None)){
                var bytes=Utf8.GetBytes(text);f.Write(bytes,0,bytes.Length);f.Flush(true);
            }
            if(File.Exists(destination))File.Replace(temporary,destination,null);else File.Move(temporary,destination);
        }
        void Parents(string path) {
            var missing=new Stack<string>();var p=Path.GetDirectoryName(path);
            while(!Directory.Exists(p)){missing.Push(p);p=Path.GetDirectoryName(p);}
            while(missing.Count>0){p=missing.Pop();Plain(p);Directory.CreateDirectory(p);journal.Directories.Add(p);Save();}
        }
        internal void Install(Action<int> step) {
            journal.State="installing";Save();int index=0;
            foreach(var change in journal.Changes){
                var path=At(root,change.Path);var backup=At(Path.Combine(work,"old"),change.Path);
                Need(change.Existed?File.Exists(path) && UpdateAgent.HashFile(path)==change.OldHash:!File.Exists(path) && !Directory.Exists(path));
                Parents(path);Directory.CreateDirectory(Path.GetDirectoryName(backup));
                if(change.NewHash==null)File.Move(path,backup);
                else{
                    var incoming=At(Path.Combine(work,"new"),change.Path);
                    if(change.Existed)File.Replace(incoming,path,backup,false);else File.Move(incoming,path);
                    Need(UpdateAgent.HashFile(path)==change.NewHash);
                }
                if(step!=null)step(++index);
            }
            journal.State="installed";Save();
        }
        internal void Commit() {journal.State="committed";Save();committed=true;}
        internal void RevertCommitted() {committed=false;Rollback();}
        internal void Rollback() {
            if(committed || rolledBack)return;
            bool complete=true;
            foreach(var change in journal.Changes.AsEnumerable().Reverse())try{
                var path=At(root,change.Path);var backup=At(Path.Combine(work,"old"),change.Path);
                if(File.Exists(backup)){
                    Need(UpdateAgent.HashFile(backup)==change.OldHash);
                    if(File.Exists(path)){
                        var current=UpdateAgent.HashFile(path);Need(current==change.NewHash || current==change.OldHash);
                        if(current==change.OldHash)File.Delete(backup);else File.Replace(backup,path,null,false);
                    }else File.Move(backup,path);
                }else if(!change.Existed && File.Exists(path)){
                    Need(UpdateAgent.HashFile(path)==change.NewHash);File.Delete(path);
                }
            }catch{complete=false;}
            foreach(var dir in journal.Directories.AsEnumerable().Reverse())try{if(Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any())Directory.Delete(dir);}catch{complete=false;}
            if(!complete){journal.State="rollback-required";Save();throw new IOException("update rollback requires recovery; backups retained");}
            rolledBack=true;Cleanup();
        }
        void Cleanup() {
            // Delete only this transaction's staging tree, never a user directory.
            Plain(work);Need(Path.GetDirectoryName(work)==root && Regex.IsMatch(Path.GetFileName(work),@"\A\.civ6-update-[0-9a-f]{32}\z"));
            if(!Directory.Exists(work))return;
            foreach(var item in Directory.EnumerateFileSystemEntries(work,"*",SearchOption.AllDirectories))Plain(item);
            foreach(var file in Directory.GetFiles(work,"*",SearchOption.AllDirectories))File.Delete(file);
            foreach(var dir in Directory.GetDirectories(work,"*",SearchOption.AllDirectories).OrderByDescending(x=>x.Length))Directory.Delete(dir);
            Directory.Delete(work);
        }
        public void Dispose(){
            if(!committed && !rolledBack)Rollback();
            else if(committed)try{Cleanup();}catch(IOException){}catch(UnauthorizedAccessException){}
        }
        internal static void Recover(string targetRoot,string pending) {
            var root=Path.GetFullPath(targetRoot);var work=Path.GetFullPath(pending);Plain(work);
            Need(Path.GetDirectoryName(work)==root && Regex.IsMatch(Path.GetFileName(work),@"\A\.civ6-update-[0-9a-f]{32}\z"));
            var file=Path.Combine(work,"journal.json");Need(new FileInfo(file).Length<=2*1024*1024);
            var journal=new JavaScriptSerializer().Deserialize<Journal>(File.ReadAllText(file,Utf8));
            Need(journal!=null && journal.Changes!=null && journal.Changes.Count<=4096 && journal.Directories!=null);
            var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach(var c in journal.Changes)Need(Managed(c.Path) && names.Add(c.Path) && (!c.Existed || Hex(c.OldHash)) && (c.NewHash==null || Hex(c.NewHash)));
            foreach(var dir in journal.Directories)Need(Path.GetFullPath(dir).StartsWith(root+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase));
            var transaction=new Transaction(root,work,journal);
            if(journal.State=="committed"){transaction.committed=true;transaction.Cleanup();}else transaction.Rollback();
        }
    }
    internal static void RecoverAll(string root) {
        Plain(root);
        foreach(var work in Directory.GetDirectories(root,".civ6-update-*"))Transaction.Recover(root,work);
    }
}
