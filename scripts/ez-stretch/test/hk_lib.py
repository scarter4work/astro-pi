"""Offline reimplementation of EZHazeKill 1.0.2 + proposed v1.1 math, and metrics."""
import numpy as np
from scipy import ndimage as ndi

# ---------------------------------------------------------------- v1.0.2 exactly
def hk_v102(img, target=0.05):
    """$T - median_c + target, PixelMath truncate=true -> clip to [0,1]."""
    med = np.median(img.reshape(-1, img.shape[2]), axis=0)
    out = np.clip(img - med + target, 0.0, 1.0)
    return out, dict(sky=med, offset=med - target)

# ---------------------------------------------------------------- robust sky
def tile_stats(img, tile=64):
    H, W, C = img.shape
    ny, nx = H // tile, W // tile
    t = img[:ny * tile, :nx * tile].reshape(ny, tile, nx, tile, C).transpose(0, 2, 1, 3, 4).reshape(ny * nx, tile * tile, C)
    med = np.median(t, axis=1)                                   # (ntiles, C)
    mad = 1.4826 * np.median(np.abs(t - med[:, None, :]), axis=1)
    return med, mad

def robust_sky(img, tile=64, q=25):
    """Proposed estimator: per-channel median of the tile medians of the darkest q% tiles
    (darkness ranked by tile luminance). sigma = median tile MAD of the same tiles."""
    med, mad = tile_stats(img, tile)
    L = med.mean(axis=1)
    sel = L <= np.percentile(L, q)
    return np.median(med[sel], axis=0), np.median(mad[sel], axis=0)

def ref_sky(img, tile=64):
    """Evaluation reference ('darkest-region sky'): darkest 5% tiles. Independent cut from the estimator."""
    med, mad = tile_stats(img, tile)
    L = med.mean(axis=1)
    sel = L <= np.percentile(L, 5)
    return np.median(med[sel], axis=0), np.median(mad[sel], axis=0), med, sel

# ---------------------------------------------------------------- proposed v1.1
def soft_floor(u, eps):
    """Identity above eps, smooth exponential toe below: monotonic, never < 0, C1 at eps."""
    return np.where(u >= eps, u, eps * np.exp(np.minimum(u, eps) / eps - 1.0))

def hk_v11(img, amount=0.5, neutralize=1.0, target=0.05, floor_sigma=4.0, toe=True, tile=64, q=10):
    sky, sig = robust_sky(img, tile, q)
    skyL = sky.mean()
    cast = sky - skyL                                            # per-channel colour cast at the sky
    tgt = max(target, floor_sigma * sig.max())                   # never push the sky into its own noise
    ped = max(0.0, skyL - tgt)                                   # never raise the pedestal
    off = neutralize * cast + amount * ped
    u = img - off
    if toe:
        eps = max(1e-6, 2.0 * sig.max())
        u = soft_floor(u, eps)
    return np.clip(u, 0.0, 1.0), dict(sky=sky, sigma=sig, target=tgt, ped=ped, offset=off)

# ---------------------------------------------------------------- linear-domain prep
def extract_gradient(img, tile=64, deg=3, iters=4, k=2.0):
    """GraXpert-like: polynomial background on tile medians with object rejection,
    subtracted, model median added back (pedestal kept)."""
    H, W, C = img.shape
    med, mad = tile_stats(img, tile)
    ny, nx = H // tile, W // tile
    yy, xx = np.mgrid[0:ny, 0:nx]
    yc = ((yy + 0.5) * tile / H * 2 - 1).ravel(); xc = ((xx + 0.5) * tile / W * 2 - 1).ravel()
    terms = [(i, j) for i in range(deg + 1) for j in range(deg + 1 - i)]
    A = np.stack([xc ** i * yc ** j for i, j in terms], 1)
    keep = np.ones(len(xc), bool)
    L = med.mean(1)
    keep &= L <= np.percentile(L, 70)
    for _ in range(iters):
        coef, *_ = np.linalg.lstsq(A[keep], L[keep], rcond=None)
        r = L - A @ coef
        s = 1.4826 * np.median(np.abs(r[keep]))
        keep = r < k * s
    Y, X = np.mgrid[0:H, 0:W].astype(np.float32)
    Yn = (Y + 0.5) / H * 2 - 1; Xn = (X + 0.5) / W * 2 - 1
    out = np.empty_like(img)
    for c in range(C):
        coef, *_ = np.linalg.lstsq(A[keep], med[keep, c], rcond=None)
        model = sum(cf * Xn ** i * Yn ** j for cf, (i, j) in zip(coef, terms))
        out[..., c] = img[..., c] - model + np.median(model)
    return out.astype(np.float32), keep.mean()

# ---------------------------------------------------------------- STF autostretch
def mtf(m, x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0, 0, np.where(x >= 1, 1, (m - 1) * x / ((2 * m - 1) * x - m)))

def stf_params(img, shadows=-2.8, tbg=0.25, linked=True):
    """PI AutoSTF. linked: average of per-channel median/MADn; unlinked: per channel."""
    s = img[::2, ::2].reshape(-1, img.shape[2])
    med = np.median(s, 0); madn = 1.4826 * np.median(np.abs(s - med), 0)
    if not linked:
        c0 = np.maximum(0.0, med + shadows * madn)
        m = np.array([float(mtf(tbg, md - c)) for md, c in zip(med, c0)])
        return c0, m
    med, madn = med.mean(), madn.mean()
    c0 = max(0.0, med + shadows * madn)
    m = float(mtf(tbg, med - c0))
    return c0, m

def apply_stf(img, c0, m):
    return mtf(m, (img - c0) / (1 - c0))

# ---------------------------------------------------------------- metrics
def masks(before, sky_ref, sig_ref):
    L = before.mean(2)
    Ls = ndi.gaussian_filter(L, 3)
    skyL = sky_ref.mean()
    # noise of the smoothed lum in sky: measure from the darkest region residual
    resid = Ls - ndi.gaussian_filter(L, 25)
    sig_s = 1.4826 * np.median(np.abs(resid - np.median(resid)))
    E = Ls - skyL
    stars = ndi.binary_dilation(L - ndi.median_filter(L[::1, ::1], size=9) > 15 * sig_ref.mean(), iterations=4)
    faint = (E > 5 * sig_s) & (E < 30 * sig_s) & ~stars
    bright = (E >= 30 * sig_s) & ~stars
    bg = (np.abs(E) < 1.0 * sig_s) & ~stars
    return dict(faint=faint, bright=bright, bg=bg, stars=stars, sig_s=sig_s)

def angle(a, b):
    a = np.asarray(a, float); b = np.asarray(b, float)
    return float(np.degrees(np.arccos(np.clip(a @ b / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-30), -1, 1))))

def metrics(before, after, M, sky_ref, after_ref_level):
    """after_ref_level: per-channel level the (corrected) image calls 'background' (neutral)."""
    r = {}
    r["clip0_%"] = 100 * float((after <= 0).any(2).mean())
    r["clip0_faint_%"] = 100 * float((after[M["faint"]] <= 0).any(1).mean())
    r["clip1_%"] = 100 * float((after >= 1).any(2).mean())
    b_f = before[M["faint"]].mean(0) - sky_ref
    a_f = after[M["faint"]].mean(0) - after_ref_level
    r["faint_retained_%"] = (100 * a_f / b_f).round(1).tolist()
    r["faint_lum_retained_%"] = round(float(100 * a_f.mean() / b_f.mean()), 1)
    r["faint_rgb_before"] = (b_f / b_f.sum()).round(3).tolist()
    r["faint_rgb_after"] = (a_f / a_f.sum()).round(3).tolist() if a_f.sum() > 0 else None
    r["faint_hue_shift_deg"] = round(angle(b_f, a_f), 1)
    b_b = before[M["bright"]].mean(0) - sky_ref
    a_b = after[M["bright"]].mean(0) - after_ref_level
    r["bright_hue_shift_deg"] = round(angle(b_b, a_b), 1)
    return r
