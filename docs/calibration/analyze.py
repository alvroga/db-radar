import pandas as pd, numpy as np, glob, os, re

DIR = "/Users/dit/Documents/PlatformIO/Projects/cc-radar/docs/calibration"

FILES = {
 '006': 'cal_006_flat360.csv',
 '007': 'cal_007_phone360.csv',
 '008': 'cal_008_phone360-actually flat360.csv',
 '009': 'cal_009_walk-straight flat.csv',
 '010': 'cal_010_walk-straight phone.csv',
 '011': 'cal_011_stand-still.csv',
 '012': 'cal_012_disturbance.csv',
 '013': 'cal_013_freeform.csv',
 '014': 'cal_014_disturbance.csv',
 '015': 'cal_015_shake-100hz.csv',
 '016': 'cal_016_shake-100hz.csv',
 '017': 'cal_017_flat360-rotate left.csv',
 '018': 'cal_018_flat360-rotate right.csv',
 '019': 'cal_019_phone360-rotate left.csv',
 '020': 'cal_020_phone360-rotate right.csv',
 '021': 'cal_021_walk-straight -discard.csv',
 '022': 'cal_022_walk-straight-discard.csv',
 '023': 'cal_023_stand-still.csv',
}

def read_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            if not line.startswith('#'):
                break
            line = line.strip('# \n')
            for kv in line.split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    meta[k] = v
    return meta

def read_footer(path):
    meta = {}
    with open(path) as f:
        lines = f.readlines()
    for line in lines[-6:]:
        if line.startswith('#'):
            line = line.strip('# \n')
            for kv in line.split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    meta[k] = v
    return meta

def load(key):
    path = os.path.join(DIR, FILES[key])
    df = pd.read_csv(path, comment='#')
    meta = read_meta(path)
    foot = read_footer(path)
    return df, meta, foot

def circle_fit(x, y):
    x = np.asarray(x, dtype=float); y = np.asarray(y, dtype=float)
    A = np.column_stack([x, y, np.ones_like(x)])
    b = x**2 + y**2
    sol, *_ = np.linalg.lstsq(A, b, rcond=None)
    D, E, F = sol
    cx0, cy0 = D/2, E/2
    r = np.sqrt(cx0**2 + cy0**2 + F)
    radii = np.sqrt((x-cx0)**2 + (y-cy0)**2)
    residual_std = radii.std()
    pts = np.column_stack([x - x.mean(), y - y.mean()])
    _, s, _ = np.linalg.svd(pts, full_matrices=False)
    axis_ratio = s[0]/s[1] if s[1] > 0 else np.nan
    return dict(center=(cx0, cy0), radius=r, residual_std=residual_std,
                residual_pct=100*residual_std/r if r else np.nan, axis_ratio=axis_ratio, n=len(x))

def circ_diff(a, b):
    return (a - b + 180) % 360 - 180

def circular_std_deg(angles_deg):
    rad = np.deg2rad(angles_deg)
    C, S = np.cos(rad).mean(), np.sin(rad).mean()
    R = np.hypot(C, S)
    return np.rad2deg(np.sqrt(-2*np.log(R))) if R > 0 else np.nan

def trim(df, frac=0.05):
    n = len(df)
    k = max(1, int(n*frac))
    return df.iloc[k:n-k].reset_index(drop=True)

print("="*100)
print("1+3. H0 stability across flat360 samples (006, 008, 017, 018)")
print("="*100)
for k in ['006', '008', '017', '018']:
    df, meta, foot = load(k)
    d = trim(df)
    print(f"{k} ({FILES[k][:30]:30s}) h_mag mean={d.h_mag.mean():8.1f} std={d.h_mag.std():6.2f} "
          f"(n={len(d)}, calib={meta.get('calibrated')}, cal=({meta.get('compass_cal_x')},{meta.get('compass_cal_y')}))")

print()
print("="*100)
print("2. Tilt threshold: flat vs phone-style h_mag")
print("="*100)
flat_h = []
for k in ['006', '008']:
    df, *_ = load(k)
    flat_h.append(trim(df).h_mag.mean())
phone_h = []
for k in ['007', '019', '020']:
    df, *_ = load(k)
    phone_h.append(trim(df).h_mag.mean())
flat_mean = np.mean(flat_h)
phone_mean = np.mean(phone_h)
print(f"flat360 h_mag mean:  {flat_mean:.1f} (samples 006,008: {[f'{v:.1f}' for v in flat_h]})")
print(f"phone360 h_mag mean: {phone_mean:.1f} (samples 007,019,020: {[f'{v:.1f}' for v in phone_h]})")
print(f"drop: {flat_mean-phone_mean:.1f} ({100*(flat_mean-phone_mean)/flat_mean:.1f}%)")

print()
print("="*100)
print("3/4. Circle fit (ripple residual, axis ratio) per flat360/phone360 sample")
print("="*100)
for k in ['006', '008', '017', '018', '007', '019', '020']:
    df, *_ = load(k)
    d = trim(df)
    fit = circle_fit(d.cx, d.cy)
    print(f"{k} ({FILES[k][:32]:32s}) r={fit['radius']:7.1f} resid_std={fit['residual_std']:6.2f} "
          f"resid%={fit['residual_pct']:5.2f}%  axis_ratio={fit['axis_ratio']:.3f}  n={fit['n']}")

print()
print("="*100)
print("5. Tilt bias while walking: heading_true vs GPS course (samples 009 flat, 010 phone, 021/022 discard?)")
print("="*100)
for k in ['009', '010', '021', '022']:
    df, meta, foot = load(k)
    d = df[(df.fix_valid == 1) & (df.speed_kn > 1.0)]
    if len(d) < 5:
        print(f"{k}: not enough valid moving GPS fixes (n={len(d)})")
        continue
    diffs = circ_diff(d.heading_true, d.course)
    az_mean = d.az.mean()
    ax_mean = d.ax.mean()
    ay_mean = d.ay.mean()
    print(f"{k} ({FILES[k][:28]:28s}) n={len(d):4d} mean_diff={diffs.mean():7.2f} std={diffs.std():6.2f} "
          f"median_speed_kn={d.speed_kn.median():.2f}  az_mean={az_mean:7.0f} ax_mean={ax_mean:7.0f} ay_mean={ay_mean:7.0f}")

print()
print("="*100)
print("6. Noise floor at stand-still (011, 023)")
print("="*100)
for k in ['011', '023']:
    df, *_ = load(k)
    d = trim(df)
    print(f"{k} ({FILES[k][:28]:28s}) h_mag std={d.h_mag.std():.3f}  heading_true circ_std={circular_std_deg(d.heading_true):.3f} deg  "
          f"heading range={d.heading_true.max()-d.heading_true.min():.2f} deg")

print()
print("="*100)
print("8. Freeform 3D coverage (013)")
print("="*100)
df, meta, foot = load('013')
print(f"cx range: [{df.cx.min():.0f}, {df.cx.max():.0f}]  span={df.cx.max()-df.cx.min():.0f}")
print(f"cy range: [{df.cy.min():.0f}, {df.cy.max():.0f}]  span={df.cy.max()-df.cy.min():.0f}")
print(f"cz range: [{df.cz.min():.0f}, {df.cz.max():.0f}]  span={df.cz.max()-df.cz.min():.0f}")
r3 = np.sqrt(df.cx**2 + df.cy**2 + df.cz**2)
print(f"3D radius |m|: mean={r3.mean():.1f} std={r3.std():.1f} ({100*r3.std()/r3.mean():.1f}% CV)")
# spherical coverage: elevation angle from z
elev = np.degrees(np.arcsin(df.cz / r3))
az = np.degrees(np.arctan2(df.cy, df.cx))
print(f"elevation angle range: [{elev.min():.1f}, {elev.max():.1f}] deg (full sphere = [-90,90])")
print(f"azimuth angle range: [{az.min():.1f}, {az.max():.1f}] deg (full circle = [-180,180])")

print()
print("="*100)
print("9. Shake-100hz samples (015, 016): actual rate + I2C health + accel content")
print("="*100)
for k in ['015', '016']:
    df, meta, foot = load(k)
    dt = df.ms.diff().dropna()
    dur_s = (df.ms.iloc[-1]-df.ms.iloc[0])/1000
    rate = len(df)/dur_s
    i2c_fail_delta = int(foot.get('i2c_end_fails', -1)) - int(meta.get('i2c_start_fails', -1))
    i2c_ops_delta = int(foot.get('i2c_end_ops', -1)) - int(meta.get('i2c_start_ops', -1))
    print(f"{k}: n={len(df)} duration={dur_s:.1f}s actual_rate={rate:.1f}Hz "
          f"dt_mean={dt.mean():.2f}ms dt_std={dt.std():.2f}ms dt_max={dt.max():.1f}ms")
    print(f"     i2c ops during recording={i2c_ops_delta} fails={i2c_fail_delta} "
          f"({'clean' if i2c_fail_delta==0 else 'FAILURES'})")
    print(f"     footer: {foot}")
    accel_mag = np.sqrt(df.ax**2+df.ay**2+df.az**2)
    print(f"     |accel| mean={accel_mag.mean():.0f} std={accel_mag.std():.0f} (lsb, 8192 lsb/g)")
    gyro_mag = np.sqrt(df.gx**2+df.gy**2+df.gz**2)
    print(f"     |gyro| mean={gyro_mag.mean():.0f} std={gyro_mag.std():.0f} (lsb, 128 lsb/dps)")

print()
print("="*100)
print("Body-shake spectrum: FFT of heading_true, sample 010 (walking, phone-style, 10Hz)")
print("="*100)
df, *_ = load('010')
d = trim(df, 0.03)
t = (d.ms - d.ms.iloc[0]).values / 1000.0
h = np.unwrap(np.deg2rad(d.heading_true.values))
h_detrend = h - np.polyval(np.polyfit(t, h, 1), t)
fs = 1.0/np.median(np.diff(t))
n = len(h_detrend)
freqs = np.fft.rfftfreq(n, d=1/fs)
spec = np.abs(np.fft.rfft(h_detrend * np.hanning(n)))
top = np.argsort(spec)[::-1][:8]
top = sorted([i for i in top if freqs[i] > 0.05])
print(f"fs={fs:.2f}Hz n={n} nyquist={fs/2:.2f}Hz")
print("dominant frequency components (Hz, amplitude):")
for i in top:
    print(f"  {freqs[i]:.3f} Hz  amp={spec[i]:.3f}")
print(f"heading residual std after detrend: {np.degrees(h_detrend).std():.2f} deg")

print()
print("="*100)
print("Rotate-left vs rotate-right sanity check (017 vs 018 flat, 019 vs 020 phone)")
print("="*100)
for a, b, label in [('017', '018', 'flat360'), ('019', '020', 'phone360')]:
    da, *_ = load(a); db, *_ = load(b)
    fa = circle_fit(trim(da).cx, trim(da).cy)
    fb = circle_fit(trim(db).cx, trim(db).cy)
    print(f"{label}: left(#{a}) resid%={fa['residual_pct']:.2f} axis_ratio={fa['axis_ratio']:.3f}  "
          f"vs right(#{b}) resid%={fb['residual_pct']:.2f} axis_ratio={fb['axis_ratio']:.3f}")

print()
print("="*100)
print("5b. 021/022 reclassified by accelerometer (az/ay split) instead of discarded")
print("="*100)
for fn, label in [('009', '009 flat/N'), ('010', '010 phone/N'), ('021', '021 ?/S'), ('022', '022 ?/S')]:
    df, *_ = load(fn)
    d = df[(df.fix_valid == 1) & (df.speed_kn > 1.0)]
    diffs = circ_diff(d.heading_true, d.course)
    tilt_deg = np.degrees(np.arccos(np.clip((d.az*-7700)/(np.sqrt(d.ax**2+d.ay**2+d.az**2)*7700), -1, 1)))
    print(f"{label:12s} n={len(d):4d} diff_mean={diffs.mean():8.2f} diff_std={diffs.std():6.2f}  "
          f"est_tilt_from_flat={tilt_deg.mean():5.1f}deg  az={d.az.mean():7.0f} ay={d.ay.mean():7.0f} ax={d.ax.mean():6.0f}")

print()
print("="*100)
print("9b. Shake spectrum band energy, accel magnitude (015, 016)")
print("="*100)
for k in ['015', '016']:
    df, *_ = load(k)
    n = len(df); kk = int(n*0.03)
    d = df.iloc[kk:n-kk]
    t = (d.ms - d.ms.iloc[0]).values / 1000.0
    fs = 1.0/np.median(np.diff(t))
    amag = np.sqrt(d.ax**2+d.ay**2+d.az**2).values
    amag = amag - amag.mean()
    m = len(amag)
    freqs = np.fft.rfftfreq(m, d=1/fs)
    spec = np.abs(np.fft.rfft(amag*np.hanning(m)))
    total = spec[freqs > 0.1].sum()
    bands = [(0.1, 1), (1, 3), (3, 5), (5, 10), (10, 20), (20, 50)]
    print(f"{k}: fs={fs:.1f}Hz n={m}")
    for lo, hi in bands:
        mask = (freqs >= lo) & (freqs < hi)
        print(f"   {lo:5.1f}-{hi:5.1f} Hz: {100*spec[mask].sum()/total:5.1f}% of energy")
    top = sorted([i for i in np.argsort(spec)[::-1][:5] if freqs[i] > 0.1])
    print("   peak freqs:", [f"{freqs[i]:.2f}Hz" for i in top])

print()
print("="*100)
print("Heading spectrum on FLAT walk (009) — cleaner signal than tilted 010")
print("="*100)
df, *_ = load('009')
n = len(df); k = int(n*0.03)
d = df.iloc[k:n-k]
t = (d.ms - d.ms.iloc[0]).values / 1000.0
h = np.unwrap(np.deg2rad(d.heading_true.values))
resid = h - np.polyval(np.polyfit(t, h, 1), t)
fs = 1.0/np.median(np.diff(t))
m = len(resid)
freqs = np.fft.rfftfreq(m, d=1/fs)
spec = np.abs(np.fft.rfft(resid*np.hanning(m)))
top = sorted([i for i in np.argsort(spec)[::-1][:6] if freqs[i] > 0.05])
print(f"fs={fs:.2f} n={m} residual_std_deg={np.degrees(resid).std():.2f}")
for i in top:
    print(f"  {freqs[i]:.3f} Hz amp={spec[i]:.3f}")
