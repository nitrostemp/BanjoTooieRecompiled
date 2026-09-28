"""Avoid rebuilding byte-identical regenerated files; never suppress a change."""
import hashlib,os
def capture_output_times(app):
    result={}
    for directory in [app/'generated',app/'config']:
        for path in directory.iterdir():
            if path.is_file():
                stat=path.stat()
                result[path]=(hashlib.sha256(path.read_bytes()).digest(),stat.st_atime_ns,stat.st_mtime_ns)
    return result
def restore_unchanged_times(snapshot):
    count=0
    for path,(digest,atime,mtime) in snapshot.items():
        if path.is_file() and hashlib.sha256(path.read_bytes()).digest()==digest:
            os.utime(path,ns=(atime,mtime));count+=1
    return count
