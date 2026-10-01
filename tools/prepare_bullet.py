"""Build one private RL-compatible Bullet tree for native and cross Windows builds."""
import shutil
from pathlib import Path
PATCHED=('LinearMath/btVector3.h','LinearMath/btMatrix3x3.h','BulletCollision/CollisionShapes/btCollisionShape.h')
def prepare(source,destination):
    source=Path(source);destination=Path(destination)
    for file in (source/'src').rglob('*'):
        if not file.is_file():continue
        relative=file.relative_to(source/'src');target=destination/'src'/relative
        if relative.as_posix() in PATCHED:continue
        if not target.exists() or target.stat().st_mtime_ns!=file.stat().st_mtime_ns:
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,target)
    for name in PATCHED:
        content=(source/'src'/name).read_text().replace(', 0x80)',', 0)')
        if name.endswith('btCollisionShape.h'):
            original='virtual void getAabb(const btTransform& t,btVector3& aabbMin,btVector3& aabbMax) const =0;'
            if content.count(original)!=1:raise ValueError('Unexpected Bullet shape ABI; refusing to build.')
            content=content.replace(original,original+'\n\tvirtual void getAabbSlow(const btTransform& t,btVector3& aabbMin,btVector3& aabbMax) const { getAabb(t,aabbMin,aabbMax); }')
        target=destination/'src'/name
        if not target.exists() or target.read_text()!=content:
            target.parent.mkdir(parents=True,exist_ok=True);target.write_text(content)
    return destination
