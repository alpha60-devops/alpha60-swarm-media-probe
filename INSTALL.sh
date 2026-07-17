# Install dependencies on Fedora 43
sudo dnf install -y mediainfo ffmpeg cmake gcc-c++ pkgconfig libtorrent-rasterbar-devel rapidjson-devel

# Clone or create the project structure
mkdir -p media_enrichment/src media_enrichment/scripts media_enrichment/output
# (Place all files in their respective directories)

# Make scripts executable
chmod +x media_enrichment/scripts/run_enrichment.sh

# Run the pipeline
cd media_enrichment
./scripts/run_enrichment.sh /path/to/torrent/dir ./output/enriched.json