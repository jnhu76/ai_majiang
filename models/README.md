# Model packaging (no binary weights in Git)

The production build must bundle tested MNN-Pocket-0.3B and an appropriately licensed sherpa-onnx voice model. Keep model files out of Git and record model SHA-256, runtime commit, quantization, tokenizer, voice assets and licenses in your experiment manifest. There is no online download at runtime. Unverified model/runtime pairs must not be marked supported.
