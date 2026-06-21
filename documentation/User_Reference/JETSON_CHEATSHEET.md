install tool

python3 -m pip install --use-pep517 -e .

pip install -e edge/edge-receiver

rsync -avz --progress smartfires@10.8.184.94:/mnt/nvme_drive/data/ ./data/
