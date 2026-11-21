"""Converts a binary STL model into Signal's compact embedded mesh (assets/<name>.mesh).

    python tools/mesh/stl_to_mesh.py <model.stl> assets/cyberskull.mesh [--grid 128]

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

The model and its .mesh are third-party assets: they stay out of git (assets/ is ignored). The build
embeds assets/cyberskull.mesh when it exists; without it Signal falls back to its procedural skull.
Needs numpy.
"""
import argparse
import numpy as np

def load(p):
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

def convert(src, N):
    V, F = load(src)
    V = np.stack([V[:,0], V[:,2], -V[:,1]], 1)            # z-up print bed -> y up, face toward +z
    c = (V.min(0)+V.max(0))/2; V = (V-c)/((V.max(0)-V.min(0)).max()/2)
    vn = vertex_normals(V, F)
    cav = cavity(V, F, vn, 96)
    X, G, cid = cluster(V, F, N)
    m = len(X)
    cv = np.bincount(cid, weights=cav, minlength=m)/np.bincount(cid, minlength=m)
    XN = vertex_normals(X, G)
    return X, G, XN, cv


def eye_mask(X):
    # The two orbits, seen from the front: deep surface inside an ellipse around each socket.
    e = ((np.abs(X[:, 0]) - 0.30) / 0.17) ** 2 + ((X[:, 1] - 0.03) / 0.12) ** 2
    return np.clip(1 - e, 0, 1) ** 0.5 * np.clip((0.5 - X[:, 2]) / 0.15, 0, 1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stl'); ap.add_argument('out'); ap.add_argument('--grid', type=int, default=128)
    a = ap.parse_args()
    X, G, XN, cv = convert(a.stl, a.grid)
    if len(X) > 65535: print('warning: %d vertices, using 32-bit indices' % len(X))
    pos = np.round(np.clip(np.concatenate([X, np.clip(cv / 0.01, -1, 1)[:, None]], 1), -1, 1) * 32767).astype('<i2')
    nrm = np.round(np.clip(np.concatenate([XN, eye_mask(X)[:, None]], 1), -1, 1) * 127).astype('i1')
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
