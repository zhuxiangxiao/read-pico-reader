/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：双板共用阅读位置采样；绘制路径不执行网络请求。
 * English: Shared reader sampling; rendering never performs network requests.
 */
static bool wr_reader_text_page(void){
    return s_view==READING&&s_text&&!s_clear_confirm&&s_reader_panel==READER_PANEL_NONE&&
        s_page<book_layout_page_count()&&!book_layout_page_image_count(s_page);
}
static bool wr_reader_text_turn(int dir){
    if(!wr_reader_text_page())return false;
    if(dir>0)return s_page+1<book_layout_page_count()&&!book_layout_page_image_count(s_page+1);
    return dir<0&&s_page&&!book_layout_page_image_count(s_page-1);
}
static void wr_reader_move(void){
    if(!s_text||!book_path_is_weread(s_path)||!book_layout_page_count())return;
    size_t offset=reader_page_offset(s_page);if(offset>s_text_len)offset=s_text_len;
    weread_reader_navigated(s_path,s_chapter,(uint32_t)offset,percent(s_page),s_text+offset);
}
static void wr_reader_sample(uint64_t now, bool active) {
    static uint64_t sampled;
    if (!s_text || !book_path_is_weread(s_path) || !book_layout_page_count()) return;
    if (active && now >= sampled && now - sampled < 200) return;
    sampled = now;
    size_t offset = reader_page_offset(s_page);
    if (offset > s_text_len) offset = s_text_len;
    weread_reader_tick(s_path,s_chapter,(uint32_t)offset,percent(s_page),s_text+offset,now,active);
}
