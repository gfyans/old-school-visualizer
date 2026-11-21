"""Converts a binary STL or a 3MF model into Signal's compact embedded mesh (assets/<name>.mesh).

    python tools/mesh/stl_to_mesh.py <model.stl|.3mf> assets/cyberskull.mesh [--grid 128]
        [--turn <degrees about the vertical>] [--tilt <degrees about x>] [--eyes skull|none|x,y,rx,ry]

What it does:
  * welds the triangle soup into an indexed mesh and turns it upright (STL from a print bed is z-up;
    Signal wants y up with the face looking down +z), centred and scaled to fit in -1..1;
  * decimates it by vertex clustering on a grid (each cluster's vertex placed where it best fits the
    original surface: quadric error), keeping it under 65536 vertices for 16-bit indices;
  * bakes per-vertex smooth normals, a cavity term (how far the surface dips below its neighbours, at
    several scales: crevices go dark without runtime ambient occlusion) and an eye-socket mask.

Output layout (little endian):
  char[4] "VMSH", uint32 version (1), uint32 vertexCount, uint32 indexCount, uint32 indexBytes (2 or 4)
  vertexCount x { int16 x, y, z, cavity (snorm) ; int8 nx, ny, nz, eye (snorm) }   12 bytes each
  indexCount x uint16/uint32 (triangle list)

--turn / --tilt fix a model that doesn't face the front once upright. --eyes says where the glowing eyes
are (seen from the front, in the -1..1 fitted space): the skull's sockets by default, none, or an ellipse
pair at +-x, y with radii rx, ry.

The models and their .mesh files are third-party assets: they stay out of git (assets/ is ignored). The
build embeds assets/cyberskull.mesh, gashead.mesh and brain.mesh when they exist; without them Signal
falls back to its procedural subjects. Needs numpy.
"""
import argparse
import re
import zipfile
import numpy as np

def load_3mf(p):
    # Every mesh object in the package (3MF is a zip of XML; Bambu/Prusa keep meshes in 3D/Objects/).
    z = zipfile.ZipFile(p)
    Vs, Fs, base = [], [], 0
    num = r'(-?[0-9.eE+-]+)'
    for name in z.namelist():
        if not name.lower().endswith('.model'): continue
        x = z.read(name).decode('utf-8', 'replace')
        v = np.array(re.findall(r'<vertex x="' + num + '" y="' + num + '" z="' + num + '"', x), dtype=np.float64)
        f = np.array(re.findall(r'<triangle v1="(\d+)" v2="(\d+)" v3="(\d+)"', x), dtype=np.int64)
        if len(v) == 0 or len(f) == 0: continue
        Vs.append(v); Fs.append(f + base); base += len(v)
    V = np.concatenate(Vs); F = np.concatenate(Fs)
    u, inv = np.unique(np.round(V, 5), axis=0, return_inverse=True)
    return u, inv.reshape(-1)[F]

def load(p):
    if p.lower().endswith('.3mf'): return load_3mf(p)
    d = np.fromfile(p, dtype=np.uint8, offset=84)
    rec = np.dtype([('n','<f4',3),('v','<f4',(3,3)),('a','<u2')])
    t = d.view(rec)
    v = t['v'].reshape(-1,3).astype(np.float64)
    u, inv = np.unique(np.round(v, 5), axis=0, return_inverse=True)
    return u, inv.reshape(-1,3)

def face_normals(V, F):
    n = np.cross(V[F[:,1]]-V[F[:,0]], V[F[:,2]]-V[F[:,0]])
    return n  # length = 2*area

def vertex_normals(V, F):
    fn = face_normals(V, F)
    vn = np.zeros_like(V)
    for k in range(3): np.add.at(vn, F[:,k], fn)
    return vn/(np.linalg.norm(vn,axis=1,keepdims=True)+1e-12)

def cavity(V, F, vn, iters):
    # neighbour average via face adjacency, smoothed at several scales: positive = concave (crevice)
    n = len(V)
    cnt = np.zeros(n); 
    for k in range(3): np.add.at(cnt, F[:,k], 2)
    def avg(X):
        s = np.zeros_like(X)
        for a,b in [(0,1),(1,2),(2,0)]:
            np.add.at(s, F[:,a], X[F[:,b]]); np.add.at(s, F[:,b], X[F[:,a]])
        return s/cnt[:,None]
    out = np.zeros(n); P = V.copy()
    for it in range(1, iters+1):
        P = avg(P)
        if it in (2, 8, 32, 96):
            out += np.einsum('ij,ij->i', P - V, vn) / (it**0.5)
    return out

def cluster(V, F, N):
    cell = 2.0/N
    g = np.floor((V+1)/cell).astype(np.int64)
    key = (g[:,0]*(N+2)+g[:,1])*(N+2)+g[:,2]
    uk, cid = np.unique(key, return_inverse=True)
    m = len(uk)
    # quadric error placement
    fn = face_normals(V, F); area = np.linalg.norm(fn,axis=1); nn = fn/(area[:,None]+1e-20)
    d = -np.einsum('ij,ij->i', nn, V[F[:,0]])
    A = np.zeros((m,3,3)); b = np.zeros((m,3))
    nnT = nn[:,:,None]*nn[:,None,:]*area[:,None,None]
    nd = nn*(d*area)[:,None]
    for k in range(3):
        np.add.at(A, cid[F[:,k]], nnT); np.add.at(b, cid[F[:,k]], nd)
    mean = np.zeros((m,3)); cnt = np.bincount(cid, minlength=m)
    np.add.at(mean, cid, V); mean /= cnt[:,None]
    # solve A x = -b, regularised toward the mean
    lam = 1e-3*np.trace(A,axis1=1,axis2=2)[:,None,None]/3 + 1e-12
    x = np.linalg.solve(A + lam*np.eye(3), (-b + lam[:,:,0]*mean)[:,:,None])[:,:,0]
    lo = (np.floor((mean+1)/cell))*cell-1 - cell*0.5; hi = lo + cell*2
    x = np.clip(x, lo, hi)
    G = cid[F]
    ok = (G[:,0]!=G[:,1])&(G[:,1]!=G[:,2])&(G[:,0]!=G[:,2])
    G = G[ok]
    s = np.sort(G,1); _, u = np.unique(s, axis=0, return_index=True); G = G[np.sort(u)]
    return x, G, cid

def convert(src, N, turn=0.0, tilt=0.0):
    V, F = load(src)
    V = np.stack([V[:,0], V[:,2], -V[:,1]], 1)            # z-up print bed -> y up, face toward +z
    if turn:
        c, s_ = np.cos(np.radians(turn)), np.sin(np.radians(turn))
        V = np.stack([c*V[:,0] + s_*V[:,2], V[:,1], -s_*V[:,0] + c*V[:,2]], 1)
    if tilt:
        c, s_ = np.cos(np.radians(tilt)), np.sin(np.radians(tilt))
        V = np.stack([V[:,0], c*V[:,1] - s_*V[:,2], s_*V[:,1] + c*V[:,2]], 1)
    c = (V.min(0)+V.max(0))/2; V = (V-c)/((V.max(0)-V.min(0)).max()/2)
    vn = vertex_normals(V, F)
    cav = cavity(V, F, vn, 96)
    X, G, cid = cluster(V, F, N)
    m = len(X)
    cv = np.bincount(cid, weights=cav, minlength=m)/np.bincount(cid, minlength=m)
    XN = vertex_normals(X, G)
    return X, G, XN, cv


def eye_mask(X, eyes):
    # The two eyes, seen from the front: the surface inside an ellipse around each.
    if eyes == 'none': return np.zeros(len(X))
    if eyes == 'skull':   # the orbits: only the deep surface inside them
        e = ((np.abs(X[:, 0]) - 0.30) / 0.17) ** 2 + ((X[:, 1] - 0.03) / 0.12) ** 2
        return np.clip(1 - e, 0, 1) ** 0.5 * np.clip((0.5 - X[:, 2]) / 0.15, 0, 1)
    ex, ey, rx, ry = (float(t) for t in eyes.split(','))
    e = ((np.abs(X[:, 0]) - ex) / rx) ** 2 + ((X[:, 1] - ey) / ry) ** 2
    front = X[:, 2] > np.percentile(X[:, 2], 40)   # the front of the model, not the back of the head
    return np.clip(1 - e, 0, 1) ** 0.5 * front

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stl'); ap.add_argument('out'); ap.add_argument('--grid', type=int, default=128)
    ap.add_argument('--turn', type=float, default=0.0); ap.add_argument('--tilt', type=float, default=0.0)
    ap.add_argument('--eyes', default='skull')
    a = ap.parse_args()
    X, G, XN, cv = convert(a.stl, a.grid, a.turn, a.tilt)
    if len(X) > 65535: print('warning: %d vertices, using 32-bit indices' % len(X))
    pos = np.round(np.clip(np.concatenate([X, np.clip(cv / 0.01, -1, 1)[:, None]], 1), -1, 1) * 32767).astype('<i2')
    nrm = np.round(np.clip(np.concatenate([XN, eye_mask(X, a.eyes)[:, None]], 1), -1, 1) * 127).astype('i1')
    vert = np.zeros(len(X), dtype=[('p', '<i2', 4), ('n', 'i1', 4)])
    vert['p'] = pos; vert['n'] = nrm
    ib = 2 if len(X) <= 65535 else 4
    idx = G.astype('<u2' if ib == 2 else '<u4')
    with open(a.out, 'wb') as f:
        f.write(b'VMSH'); f.write(np.array([1, len(X), idx.size, ib], '<u4').tobytes())
        f.write(vert.tobytes()); f.write(idx.tobytes())
    print('%s: %d vertices, %d triangles' % (a.out, len(X), len(G)))

if __name__ == '__main__':
    main()
