# Stage 2: real Chinese TTS with sherpa-onnx v2.0.1 prebuilt win-x64 binaries.
$extern = "$PWD\..\ai_majiang-extern"  # SDK root outside the repo (see 00-setup-extern.ps1)
$env:HTTP_PROXY  = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
$s = "$extern/third_party/sherpa-onnx-extract/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release"
$m = "$extern/third_party/models/vits-icefall-zh-aishell3"
mkdir -Force benchmarks/results/tts | Out-Null
# 3 speakers x 3 mahjong lines (short / normal / long). sid indexes speakers.txt (174 speakers).
foreach ($sid in 0,1,100) {
  & "$s/bin/sherpa-onnx-offline-tts.exe" `
    --vits-model="$m/model.onnx" --vits-lexicon="$m/lexicon.txt" --vits-tokens="$m/tokens.txt" `
    --tts-rule-fars="$m/rule.far" --num-threads=2 --sid=$sid `
    --output-filename="benchmarks/results/tts/tts_sid${sid}_line1.wav" "碰！"
  & "$s/bin/sherpa-onnx-offline-tts.exe" `
    --vits-model="$m/model.onnx" --vits-lexicon="$m/lexicon.txt" --vits-tokens="$m/tokens.txt" `
    --tts-rule-fars="$m/rule.far" --num-threads=2 --sid=$sid `
    --output-filename="benchmarks/results/tts/tts_sid${sid}_line2.wav" "别磨蹭，快出牌！"
  & "$s/bin/sherpa-onnx-offline-tts.exe" `
    --vits-model="$m/model.onnx" --vits-lexicon="$m/lexicon.txt" --vits-tokens="$m/tokens.txt" `
    --tts-rule-fars="$m/rule.far" --num-threads=2 --sid=$sid `
    --output-filename="benchmarks/results/tts/tts_sid${sid}_line3.wav" "你这牌打得肥嘟嘟的，我都等了三轮了，快点出牌吧。"
}
