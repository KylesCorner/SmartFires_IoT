from setuptools import setup, find_packages

setup( 
    name="smartfires-edge", 
    version="0.1.0", 
    description="Edge telemetry ingest and packet-loss monitoring for SmartFires",
    package_dir={"":"src"},
    packages=find_packages(where="src"),
    install_requires=["pyserial>=3.5", "minimalmodbus>=2.1", "numpy>=1.24"],
    entry_points={ "console_scripts": [
            "smartfires-edge=smartfires_edge.main:main", ] },)
