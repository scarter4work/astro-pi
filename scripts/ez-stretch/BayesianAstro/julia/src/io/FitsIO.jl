"""
FITS file I/O operations for astronomical image data.

Provides functions for reading/writing FITS files and extracting metadata
from headers for use in the Bayesian stacking pipeline.
"""
module FitsIO

using FITSIO
using Dates
using ..BayesianAstro: FrameMetadata, ImageStack

export load_fits, save_fits, load_frame_sequence, get_fits_metadata
export load_fits_cube, find_fits_files, parse_fits_date

"""
    load_fits(filepath::String) -> Matrix{Float32}

Load a FITS file and return the image data as a Float32 matrix.
Handles both 2D images and 3D cubes (returns first plane).
"""
function load_fits(filepath::String)::Matrix{Float32}
    f = FITS(filepath, "r")
    try
        data = read(f[1])
        
        # Handle different dimensionalities
        if ndims(data) == 2
            return Float32.(data)
        elseif ndims(data) == 3
            # Return first channel/plane
            return Float32.(data[:, :, 1])
        else
            error("Unsupported FITS dimensionality: $(ndims(data))")
        end
    finally
        close(f)
    end
end

"""
    load_fits_cube(filepath::String) -> Array{Float32, 3}

Load a FITS file containing a 3D data cube.
"""
function load_fits_cube(filepath::String)::Array{Float32, 3}
    f = FITS(filepath, "r")
    try
        data = read(f[1])
        if ndims(data) != 3
            error("Expected 3D FITS cube, got $(ndims(data))D data")
        end
        return Float32.(data)
    finally
        close(f)
    end
end

"""
    save_fits(filepath::String, data::AbstractMatrix; header_cards=Dict())

Save image data to a FITS file with optional header cards.
"""
function save_fits(filepath::String, data::AbstractMatrix; header_cards::Dict{String,Any}=Dict{String,Any}())
    f = FITS(filepath, "w")
    try
        write(f, Float32.(data))
        
        # Add custom header cards
        for (key, value) in header_cards
            write_key(f[1], key, value)
        end
    finally
        close(f)
    end
end

"""
    save_fits(filepath::String, data::AbstractArray{T,3}; header_cards=Dict()) where T

Save 3D data cube to a FITS file.
"""
function save_fits(filepath::String, data::AbstractArray{T,3}; header_cards::Dict{String,Any}=Dict{String,Any}()) where T
    f = FITS(filepath, "w")
    try
        write(f, Float32.(data))
        
        for (key, value) in header_cards
            write_key(f[1], key, value)
        end
    finally
        close(f)
    end
end

"""
    parse_fits_date(datestr::String) -> Float64

Parse a FITS DATE-OBS string to Unix timestamp.
Supports formats:
- YYYY-MM-DDTHH:MM:SS.sss (ISO 8601)
- YYYY-MM-DD
- DD/MM/YY (old format)
"""
function parse_fits_date(datestr::String)::Float64
    datestr = strip(datestr)

    # Try ISO 8601 format: YYYY-MM-DDTHH:MM:SS.sss
    try
        if occursin('T', datestr)
            # Handle optional fractional seconds
            if occursin('.', datestr)
                dt = DateTime(datestr[1:min(23, length(datestr))], dateformat"yyyy-mm-ddTHH:MM:SS.sss")
            else
                dt = DateTime(datestr[1:min(19, length(datestr))], dateformat"yyyy-mm-ddTHH:MM:SS")
            end
            return datetime2unix(dt)
        end
    catch; end

    # Try date-only format: YYYY-MM-DD
    try
        if occursin('-', datestr) && length(datestr) >= 10
            dt = DateTime(datestr[1:10], dateformat"yyyy-mm-dd")
            return datetime2unix(dt)
        end
    catch; end

    # Try old format: DD/MM/YY
    try
        if occursin('/', datestr)
            dt = DateTime(datestr, dateformat"dd/mm/yy")
            return datetime2unix(dt)
        end
    catch; end

    return 0.0
end

"""
    get_header_value(hdr, keys...; default=nothing)

Try multiple header keys and return the first found value.
"""
function get_header_value(hdr, keys...; default=nothing)
    for key in keys
        try
            if haskey(hdr, key)
                val = hdr[key]
                if val !== nothing && val != ""
                    return val
                end
            end
        catch
            continue
        end
    end
    return default
end

"""
    get_fits_metadata(filepath::String) -> FrameMetadata

Extract metadata from FITS header to construct FrameMetadata.
Attempts to read common keywords for FWHM, background, noise, and timestamp.

# Supported Keywords
- FWHM: FWHM, SEEING, AVGFWHM
- Background: BACKGRND, SKYLEVEL, PEDESTAL, BACKGROUND
- Noise: NOISE, RDNOISE, READNOIS
- Timestamp: DATE-OBS, JD, MJD-OBS
"""
function get_fits_metadata(filepath::String)::FrameMetadata
    f = FITS(filepath, "r")
    try
        hdr = read_header(f[1])

        # Try to extract common metadata keywords
        fwhm_val = get_header_value(hdr, "FWHM", "SEEING", "AVGFWHM"; default=0.0)
        background_val = get_header_value(hdr, "BACKGRND", "SKYLEVEL", "PEDESTAL", "BACKGROUND"; default=0.0)
        noise_val = get_header_value(hdr, "NOISE", "RDNOISE", "READNOIS"; default=0.0)

        # Try to get timestamp
        timestamp = 0.0

        # Try DATE-OBS first
        date_obs = get_header_value(hdr, "DATE-OBS"; default=nothing)
        if date_obs !== nothing && date_obs != ""
            timestamp = parse_fits_date(string(date_obs))
        end

        # Fall back to Julian Date
        if timestamp == 0.0
            jd = get_header_value(hdr, "JD", "JD-OBS"; default=nothing)
            if jd !== nothing
                # Julian date to Unix timestamp
                timestamp = (Float64(jd) - 2440587.5) * 86400.0
            end
        end

        # Fall back to Modified Julian Date
        if timestamp == 0.0
            mjd = get_header_value(hdr, "MJD-OBS", "MJD"; default=nothing)
            if mjd !== nothing
                # MJD to Unix timestamp (MJD epoch is 1858-11-17)
                timestamp = (Float64(mjd) + 2400000.5 - 2440587.5) * 86400.0
            end
        end

        return FrameMetadata(
            filepath;
            fwhm=Float32(fwhm_val),
            background=Float32(background_val),
            noise=Float32(noise_val),
            weight=1.0f0,
            timestamp=timestamp
        )
    finally
        close(f)
    end
end

"""
    load_frame_sequence(filepaths::Vector{String}; extract_metadata=true) -> ImageStack

Load a sequence of FITS files into an ImageStack.

# Arguments
- `filepaths`: Vector of paths to FITS files
- `extract_metadata`: Whether to parse FITS headers for frame metadata
"""
function load_frame_sequence(filepaths::Vector{String}; extract_metadata::Bool=true)::ImageStack{Float32}
    @assert length(filepaths) > 0 "Must provide at least one file"
    
    frames = Matrix{Float32}[]
    metadata = FrameMetadata[]
    
    for (i, filepath) in enumerate(filepaths)
        @info "Loading frame $i/$(length(filepaths)): $(basename(filepath))"
        
        frame = load_fits(filepath)
        push!(frames, frame)
        
        if extract_metadata
            meta = get_fits_metadata(filepath)
        else
            meta = FrameMetadata(filepath)
        end
        push!(metadata, meta)
    end
    
    # Validate all frames have same dimensions
    ref_size = size(frames[1])
    for (i, frame) in enumerate(frames)
        if size(frame) != ref_size
            error("Frame $i has different dimensions: $(size(frame)) vs $ref_size")
        end
    end
    
    return ImageStack(frames, metadata)
end

"""
    find_fits_files(directory::String; pattern=r"\\.fits?\$"i) -> Vector{String}

Find all FITS files in a directory matching the given pattern.
"""
function find_fits_files(directory::String; pattern::Regex=r"\.fits?$"i)::Vector{String}
    files = String[]
    for entry in readdir(directory; join=true)
        if isfile(entry) && occursin(pattern, entry)
            push!(files, entry)
        end
    end
    return sort(files)
end

end # module FitsIO
