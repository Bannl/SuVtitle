# Prints C++ wide literals (\u escapes only) for the subreader smoke's sample sentences.
S = {
 "kZhHans": ["对白音量刚好，但混响尾音太长了。", "旁白下面的音乐能再压低两分贝吗？", "这一版先导出，明天再听一遍。",
             "好的，我把混响缩短到一点二秒。", "我们先从鼓的分轨开始。", "然后我们导出混音。",
             "请在第三通道给我一个稳定的信号。", "音量有点低，我再推高三个分贝。", "现在怎么样？", "这样在混音里好听多了。"],
 "kZhHant": ["對白音量剛好，但殘響太長了。", "旁白下面的音樂能再壓低兩分貝嗎？", "這一版先匯出，明天再聽一遍。",
             "好的，我把殘響縮短到一點二秒。", "我們先從鼓的分軌開始。", "然後我們匯出混音。",
             "請在第三通道給我一個穩定的訊號。", "音量有點低，我再推高三個分貝。", "現在怎麼樣？", "這樣在混音裡好聽多了。"],
 "kJa": ["今日はミックスについて話しましょう。", "ドラムの音をもう少し下げてください。", "リバーブが長すぎると思います。",
         "このバージョンを書き出しましょう。", "明日もう一度聞きます。", "ボーカルが少し小さいです。",
         "三番目のチャンネルを確認してください。", "いい感じになりました。", "もう一回お願いします。", "ありがとうございました。"],
 "kKo": ["오늘은 믹싱에 대해 이야기합시다.", "드럼 소리를 조금 줄여 주세요.", "리버브가 너무 긴 것 같아요.",
         "이 버전을 내보냅시다.", "내일 다시 들어 볼게요.", "보컬이 조금 작아요.", "세 번째 채널을 확인해 주세요.",
         "이제 좋아졌어요.", "한 번 더 부탁해요.", "감사합니다."],
 "kRu": ["Уровень диалога нормальный, но хвост реверберации слишком длинный.",
         "Можно убрать музыку на два децибела под дикторский текст?",
         "Давайте экспортируем эту версию и послушаем завтра.", "Хорошо, я укорочу реверберацию.",
         "Начнём со стемов барабанов.", "Потом сведём микс.", "Дайте ровный сигнал на третий канал.",
         "Громкость немного низкая.", "Как сейчас?", "Так в миксе звучит гораздо лучше."],
 "kFr": ["Aujourd'hui on parle du mixage de la batterie.",
         "Le niveau du dialogue est bon, mais la réverbération est trop longue.",
         "Peut-on baisser la musique de deux décibels sous la narration ?",
         "Exportons cette version et réécoutons-la demain.", "« Très bien », dit l'ingénieur du son.",
         "Ça sonne beaucoup mieux maintenant, n'est-ce pas ?", "Où est la piste de la caisse claire ?",
         "Il faut éviter les fréquences trop aiguës.", "Voilà, c'est prêt à être exporté.",
         "Merci à tous d'être venus ce matin."],
}
BS = chr(92)
def lit(s):
    o = []
    for ch in s:
        c = ord(ch)
        if 32 <= c < 127 and ch not in (BS, '"'):
            o.append(ch)
        elif c < 0x10000:
            o.append(BS + "u%04X" % c)
        else:
            o.append(BS + "U%08X" % c)
    return 'L"' + "".join(o) + '"'
for k, v in S.items():
    print("static const wchar_t* const %s[] = {" % k)
    for s in v:
        print("    " + lit(s) + ",")
    print("};")
