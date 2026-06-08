# NukeX Training Data Source Priority

This document defines the priority hierarchy for training data sources used in the NukeX 21-class segmentation model.

## Data Source Priority Order

### 1. HIGHEST PRIORITY: QNAP Mount Data (`/mnt/qnap`)

**Location:** CARTER-NAS at 192.168.68.81, mounted at `/mnt/qnap`

**Why highest priority:**
- User's own images with known equipment, processing, and quality
- Ground truth is more reliable (known targets and conditions)
- Represents actual use cases and equipment combinations
- Known provenance = better labels

**Key directories:**
- `/mnt/qnap/astro_data/` - Organized by date, contains calibrated subframes
- `/mnt/qnap/scotts_subs/` - Additional subframe data
- Raw FITS files directly in `/mnt/qnap/` (e.g., NGC281 Ha/OIII/SII/L data)

**Processing script:** `scripts/process_qnap_data.py`

**Manifest tagging:** `source_priority: 1`, `source: "qnap"`

---

### 2. SECONDARY: High-Quality External Sources

#### 2a. ESA/Hubble Archive
**Processing script:** `scripts/download_hst.py`
**Manifest tagging:** `source_priority: 2`, `source: "hst"`

#### 2b. ESO Archive
**Processing script:** `scripts/download_eso.py`
**Manifest tagging:** `source_priority: 2`, `source: "eso"`

#### 2c. AstroBin (Trusted Community)
**Manifest tagging:** `source_priority: 2`, `source: "astrobin"`

---

### 3. LOWEST PRIORITY: General Web Sources

Use sparingly and with careful validation.

**Manifest tagging:** `source_priority: 3`, `source: "web"`

---

## Implementation Guidelines

### Data Discovery Scripts

All data discovery/preparation scripts MUST:
1. Check QNAP mount first (`/mnt/qnap`)
2. Fall back to external sources only if QNAP data insufficient
3. Tag all training pairs with `source` and `source_priority` fields

### Training Manifest Format

Each training pair in manifests should include:

```json
{
  "image": "/path/to/image.png",
  "mask": "/path/to/mask.png",
  "source": "qnap",
  "source_priority": 1,
  "target_object": "M42",
  "filter": "Ha",
  "equipment": "ZWO ASI294MM Pro"
}
```

### Training Weight Considerations

When mixing data sources in training:
- Consider weighting QNAP data higher (e.g., 2x sample weight)
- Use `source_priority` field for dynamic weighting in data loaders
- Monitor validation performance separately by source

### Adding New Data

When adding new training data:
1. **First choice:** Add to QNAP collection
2. **Second choice:** Use reputable astronomical archives
3. **Last resort:** Web sources (require manual validation)

---

## Current Data Directories

### QNAP-sourced (Priority 1)
- `labeled_qnap_m27/` - M27 (Dumbbell Nebula) data
- `labeled_emission_qnap/` - Emission nebulae from QNAP
- `labeled_reflection_qnap/` - Reflection nebulae from QNAP
- `labeled_narrowband/` - Narrowband filter data

### External Sources (Priority 2-3)
- `labeled/` - Mixed sources
- `labeled_targeted/` - Specific target downloads
- `labeled_stretched/` - Pre-stretched data
- `labeled_pn/` - Planetary nebulae
- `labeled_reflection/` - Reflection nebulae
- `labeled_clusters/` - Star clusters
- `labeled_dustlanes/` - Dust lane features
- `labeled_dark_nebula/` - Dark nebulae
- `labeled_linear_noise/` - Noise pattern data

### Synthetic Data
- `synthetic/` - Generated training data
- `synthetic_extra/` - Additional synthetic
- `synthetic_artifacts/` - Generated artifacts
- `synthetic_massive/` - Large-scale synthetic
- `synthetic_weak/` - Weak class augmentation

---

## QNAP Connection

**Mount command:**
```bash
# Already in fstab, but manual mount if needed:
sudo mount -t cifs //192.168.68.81/share /mnt/qnap -o credentials=/path/to/credentials
```

**Verify mount:**
```bash
ls -la /mnt/qnap/astro_data
```

---

## Version History

- 2026-01-28: Initial documentation of data source priority hierarchy
