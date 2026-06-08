"""
Target definitions for segmentation training data collection.
Organized by the 21 segmentation classes in NukeX.
"""

# Targets organized by dominant feature type
# Each target may contain multiple classes, but is selected for its primary feature

TARGETS = {
    # === STELLAR FEATURES ===
    "star_core_halo": [
        # Bright star fields with clear core/halo structure
        "Sirius", "Vega", "Arcturus", "Capella", "Rigel",
        "Betelgeuse", "Aldebaran", "Spica", "Antares", "Pollux"
    ],

    "star_clusters": [
        # Open clusters - excellent for star field training
        "M45",      # Pleiades - reflection nebula + stars
        "M44",      # Beehive
        "NGC 869",  # Double Cluster h
        "NGC 884",  # Double Cluster chi
        "M35",      # Open cluster Gemini
        "M37",      # Open cluster Auriga
        "M38",      # Open cluster Auriga
        "NGC 7789", # Caroline's Rose
        # Globular clusters - dense star cores
        "M13",      # Great Hercules Cluster
        "M22",      # Sagittarius
        "M5",       # Serpens
        "NGC 6752", # Pavo
        "47 Tuc",   # Tucana
        "Omega Centauri",
    ],

    # === EMISSION NEBULAE ===
    "bright_emission": [
        "M42",          # Orion Nebula - iconic
        "NGC 2024",     # Flame Nebula
        "M8",           # Lagoon Nebula
        "M17",          # Omega/Swan Nebula
        "M20",          # Trifid Nebula
        "NGC 3372",     # Carina Nebula
        "NGC 7000",     # North America Nebula
        "IC 5070",      # Pelican Nebula
        "NGC 6888",     # Crescent Nebula
        "NGC 2237",     # Rosette Nebula
    ],

    "faint_emission": [
        "Sh2-129",      # Flying Bat Nebula
        "Sh2-155",      # Cave Nebula
        "Sh2-101",      # Tulip Nebula
        "IC 1396",      # Elephant's Trunk region
        "Sh2-240",      # Simeis 147 - supernova remnant
        "IC 1805",      # Heart Nebula
        "IC 1848",      # Soul Nebula
        "NGC 6992",     # Eastern Veil
        "NGC 6960",     # Western Veil
        "Sh2-132",      # Lion Nebula
    ],

    # === DARK NEBULAE ===
    "dark_nebula": [
        "Barnard 33",   # Horsehead Nebula
        "B68",          # Barnard 68 - isolated dark globule
        "LDN 1622",     # Boogeyman Nebula
        "LDN 673",      # Dark nebula in Aquila
        "Barnard 86",   # Inkspot Nebula
        "Barnard 92",   # Dark nebula in Sagittarius
        "LDN 1251",     # Dark nebula in Cepheus
        "Barnard 142",  # E Nebula part
        "Barnard 143",  # E Nebula part
        "Coal Sack",    # Southern dark nebula
    ],

    # === REFLECTION NEBULAE ===
    "reflection_nebula": [
        "M78",          # Orion reflection nebula
        "NGC 1999",     # Keyhole in Orion
        "IC 2118",      # Witch Head Nebula
        "NGC 1333",     # Reflection nebula in Perseus
        "NGC 7023",     # Iris Nebula
        "vdB 141",      # Ghost Nebula
        "IC 405",       # Flaming Star Nebula
        "NGC 2068",     # M78 region
        "Merope Nebula",# In Pleiades
        "vdB 152",      # Cepheus reflection
    ],

    # === GALAXIES ===
    "galaxy_core": [
        # Face-on spirals with bright cores
        "M31",          # Andromeda
        "M81",          # Bode's Galaxy
        "M51",          # Whirlpool
        "M101",         # Pinwheel
        "M33",          # Triangulum
        "NGC 4565",     # Needle Galaxy - edge-on core
        "M104",         # Sombrero - prominent bulge
        "NGC 2841",     # Flocculent spiral
        "M63",          # Sunflower Galaxy
        "M94",          # Bright core
    ],

    "galaxy_arm": [
        # Spirals with well-defined arms
        "M51",          # Whirlpool - classic arms
        "M101",         # Pinwheel - loose arms
        "M74",          # Perfect spiral
        "NGC 1232",     # Grand design spiral
        "M83",          # Southern Pinwheel
        "NGC 6946",     # Fireworks Galaxy
        "M61",          # Virgo spiral
        "NGC 2997",     # Southern spiral
        "M100",         # Coma spiral
        "NGC 1300",     # Barred spiral
    ],

    "galaxy_dust_lane": [
        # Edge-on or tilted galaxies with visible dust
        "NGC 891",      # Classic edge-on dust lane
        "NGC 4565",     # Needle Galaxy
        "M104",         # Sombrero - prominent dust ring
        "NGC 5866",     # Spindle Galaxy
        "M82",          # Starburst with dust
        "Centaurus A",  # NGC 5128 - dramatic dust
        "M64",          # Black Eye Galaxy
        "NGC 4631",     # Whale Galaxy
        "NGC 4217",     # Edge-on with dust
        "NGC 2683",     # UFO Galaxy
    ],

    # === PLANETARY NEBULAE (compact, good for transition zones) ===
    "planetary_nebula": [
        "M57",          # Ring Nebula
        "M27",          # Dumbbell Nebula
        "NGC 7293",     # Helix Nebula
        "NGC 6826",     # Blinking Nebula
        "M97",          # Owl Nebula
        "NGC 7662",     # Blue Snowball
        "NGC 6543",     # Cat's Eye
        "NGC 3132",     # Eight-Burst Nebula
        "NGC 2392",     # Eskimo Nebula
        "IC 418",       # Spirograph Nebula
    ],

    # === SUPERNOVA REMNANTS (complex transition zones) ===
    "snr": [
        "M1",           # Crab Nebula
        "NGC 6992",     # Veil Nebula East
        "NGC 6960",     # Veil Nebula West
        "Sh2-240",      # Simeis 147
        "IC 443",       # Jellyfish Nebula
        "Cassiopeia A", # Young SNR
        "Vela SNR",     # Southern
        "Puppis A",     # SNR
    ],

    # === MIXED/COMPLEX REGIONS (good for all classes) ===
    "complex_regions": [
        "Rho Ophiuchi",     # Stars + reflection + dark + emission
        "Orion Belt",       # IC 434 region - everything
        "Cygnus Wall",      # NGC 7000 region
        "Carina Nebula",    # NGC 3372 - all features
        "M16",              # Eagle Nebula - pillars + emission
        "IC 1396",          # Elephant's Trunk - dark + emission
        "NGC 6914",         # Reflection + emission
        "Sadr region",      # IC 1318 complex
        "NGC 281",          # Pacman - dark + emission
        "Sh2-106",          # Bipolar emission
    ],
}

# Flat list of all unique targets
ALL_TARGETS = list(set(
    target
    for category in TARGETS.values()
    for target in category
))

# Priority targets - most valuable for training (complex, well-studied)
PRIORITY_TARGETS = [
    "M42",          # Gold standard emission
    "Barnard 33",   # Iconic dark nebula
    "M51",          # Perfect spiral
    "NGC 891",      # Perfect edge-on
    "M45",          # Stars + reflection
    "Rho Ophiuchi", # Everything
    "NGC 7000",     # Large emission complex
    "M31",          # Nearby galaxy
    "M57",          # Compact planetary
    "NGC 6992",     # Filamentary SNR
]

# ESO-specific instrument preferences
ESO_INSTRUMENTS = {
    "primary": ["fors2", "omegacam"],  # Best optical imagers
    "secondary": ["wfi", "vimos"],      # Older but large archive
}

# HST instrument preferences
HST_INSTRUMENTS = [
    "ACS/WFC",      # Advanced Camera - wide field
    "WFC3/UVIS",    # Wide Field Camera 3 - UV/optical
    "WFPC2",        # Older but large archive
]

if __name__ == "__main__":
    print(f"Total unique targets: {len(ALL_TARGETS)}")
    for cat, targets in TARGETS.items():
        print(f"  {cat}: {len(targets)} targets")
