// 国际化支持 - 重构版本
const i18n = {
    currentLang: 'zh-CN',
    translations: {},
    brandConfig: null,
    ready: false,
    initPromise: null,
    translatedElements: new WeakSet(), // 跟踪已翻译的元素，避免重复
    verificationInterval: null, // 语言验证定时器
    translationAttempts: 0, // 翻译尝试次数

    async init() {
        if (this.initPromise) {
            return this.initPromise;
        }

        this.initPromise = this.initInternal();
        return this.initPromise;
    },

    async initInternal() {
        // 获取品牌配置（从全局变量）
        this.brandConfig = window.BRAND_CONFIG || {
            brand: 'SHUOXIN',
            brandNameCN: '硕芯电子',
            brandNameEN: 'SHUOXIN',
            model: 'SP501W'
        };

        // 从localStorage读取用户语言偏好
        const savedLang = localStorage.getItem('language') || 'zh-CN';
        await this.loadLanguage(savedLang);

        // 应用品牌样式
        this.applyBrandStyle();

        // 启动语言验证机制（每3秒检查一次）
        this.startLanguageVerification();
    },

    // 新增：语言验证机制，定期检查并修正语言显示
    startLanguageVerification() {
        // 清除已存在的定时器
        if (this.verificationInterval) {
            clearInterval(this.verificationInterval);
        }

        // 每3秒检查一次语言状态
        this.verificationInterval = setInterval(() => {
            this.verifyAndCorrectLanguage();
        }, 3000);
    },

    // 新增：验证并修正语言显示
    verifyAndCorrectLanguage() {
        // 检查是否有未翻译或翻译错误的元素
        const elements = document.querySelectorAll('[data-i18n]');
        let needsCorrection = false;

        elements.forEach(el => {
            const key = el.getAttribute('data-i18n');
            const expectedTranslation = this.t(key);

            // 检查元素是否可见
            if (!this.isElementVisible(el)) return;

            const currentText = this.getElementText(el);

            // 如果当前文本是翻译键本身，说明没有翻译成功
            if (currentText === key) {
                needsCorrection = true;
                console.warn(`未翻译的元素: ${key}`);
            }
            // 如果当前文本与期望翻译不匹配（排除空值和动态内容）
            else if (expectedTranslation && expectedTranslation !== key &&
                     currentText !== expectedTranslation &&
                     currentText.length > 0 && currentText.length < 100) {
                // 只修正明显的语言错误（不修正数字、状态等动态内容）
                if (this.isProbablyTranslationError(currentText, expectedTranslation)) {
                    needsCorrection = true;
                    console.warn(`翻译不匹配: ${key}, 当前="${currentText}", 期望="${expectedTranslation}"`);
                }
            }
        });

        // 如果发现需要修正的内容，重新应用翻译
        if (needsCorrection) {
            console.log('检测到语言显示错误，正在修正...');
            this.translationAttempts++;
            this.applyTranslations();

            // 如果频繁修正（5次以内），增加检查频率
            if (this.translationAttempts <= 5) {
                // 额外触发一次快速检查（1秒后）
                setTimeout(() => this.applyTranslations(), 1000);
            }
        } else {
            // 重置尝试计数
            this.translationAttempts = 0;
        }
    },

    // 新增：判断元素是否可见
    isElementVisible(el) {
        return el.offsetParent !== null || el.offsetWidth > 0 || el.offsetHeight > 0;
    },

    // 新增：获取元素的文本内容
    getElementText(el) {
        if (el.children.length > 0) {
            // 有子元素，获取直接文本节点
            let text = '';
            Array.from(el.childNodes).forEach(node => {
                if (node.nodeType === 3) {
                    text += node.textContent.trim();
                }
            });
            return text;
        } else {
            return el.textContent.trim();
        }
    },

    // 新增：判断是否可能是翻译错误
    isProbablyTranslationError(currentText, expectedTranslation) {
        // 如果当前文本是纯数字、IP地址等，不是翻译错误
        if (/^\d+(\.\d+)*$/.test(currentText)) return false;
        if (/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(currentText)) return false;

        // 如果当前是中文，期望是英文（或反之），可能是语言错误
        const isChinese = /[一-龥]/.test(currentText);
        const expectedChinese = /[一-龥]/.test(expectedTranslation);

        return isChinese !== expectedChinese;
    },

    async loadLanguage(lang) {
        try {
            this.ready = false;
            const response = await fetch(`/i18n/${lang}.json?v=${Date.now()}`, {
                cache: 'no-store'
            });
            this.translations = await response.json();

            this.translations.brandName = lang === 'zh-CN' ?
                this.brandConfig.brandNameCN : this.brandConfig.brandNameEN;

            this.currentLang = lang;
            localStorage.setItem('language', lang);
            this.ready = true;

            // 清除已翻译标记，重新翻译
            this.translatedElements = new WeakSet();
            this.applyTranslations();

            // 更新语言切换按钮
            this.updateLangButton();

            // 触发额外的翻译应用（确保动态加载的内容也被翻译）
            setTimeout(() => this.applyTranslations(), 500);
            setTimeout(() => this.applyTranslations(), 1500);

            window.dispatchEvent(new CustomEvent('languageChanged', {
                detail: { lang }
            }));
        } catch (error) {
            this.ready = false;
            console.error('Failed to load language file:', error);
        }
    },

    isReady() {
        return this.ready;
    },

    t(key, fallback) {
        if (!this.ready) {
            return fallback !== undefined ? fallback : '';
        }

        const keys = key.split('.');
        let value = this.translations;
        for (const k of keys) {
            value = value?.[k];
        }
        if (value !== undefined && value !== null && value !== '') {
            return value;
        }
        return fallback !== undefined ? fallback : key;
    },

    applyTranslations() {
        // 更新页面标题
        const title = this.t('title');
        const brandName = this.currentLang === 'zh-CN' ? '硕芯' : 'SHUOXIN';
        document.title = `${brandName}${title}`;

        // 更新所有带data-i18n属性的元素
        // 每个元素只更新自己的直接文本节点，嵌套的data-i18n子元素继续单独翻译
        const elements = document.querySelectorAll('[data-i18n]');

        elements.forEach(el => {
            const key = el.getAttribute('data-i18n');
            const translation = this.t(key);

            // 检查元素是否有子元素
            const hasChildElements = el.children.length > 0;

            if (hasChildElements) {
                // 如果有子元素，只更新直接文本节点
                this.updateTextNodes(el, translation);
            } else {
                // 如果没有子元素，直接设置textContent
                el.textContent = translation;
            }
        });

        // 更新所有带data-i18n-placeholder属性的元素
        document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
            const key = el.getAttribute('data-i18n-placeholder');
            el.placeholder = this.t(key);
        });

        // 更新导航栏标题
        const navTitles = document.querySelectorAll('.logo-title .title');
        navTitles.forEach(el => {
            const brandName = this.currentLang === 'zh-CN' ? '硕芯' : 'SHUOXIN';
            el.textContent = `${brandName}${title}`;
        });

        // 更新版本信息
        this.updateVersionInfo();

        // 更新当前页面标题（mainTitle）
        this.updateMainTitle();
    },

    // 只更新元素的直接文本节点，保留子元素
    updateTextNodes(element, text) {
        // 遍历所有直接子节点
        const childNodes = Array.from(element.childNodes);
        let textNodeFound = false;

        childNodes.forEach(node => {
            // 如果是文本节点（nodeType === 3）
            if (node.nodeType === 3) {
                const trimmedText = node.textContent.trim();
                // 只更新非空的文本节点
                if (trimmedText) {
                    node.textContent = text;
                    textNodeFound = true;
                }
            }
        });

        // 如果没有找到文本节点，在开头插入一个
        if (!textNodeFound) {
            // 在第一个子元素之前插入文本节点
            const textNode = document.createTextNode(text);
            element.insertBefore(textNode, element.firstChild);
        }
    },

    updateVersionInfo() {
        const versionDivs = document.querySelectorAll('.version');
        versionDivs.forEach(div => {
            let versionText = '';
            if (this.brandConfig.brand === 'SHUOXIN') {
                versionText = this.currentLang === 'zh-CN' ?
                    'SHUOXIN-IOT V2.0.0' : 'SHUOXIN-IOT V2.0.0';
            } else {
                versionText = 'IOT V2.0.0';
            }
            div.textContent = versionText;
        });
    },

    updateMainTitle() {
        const mainTitleElement = document.getElementById('mainTitle');
        if (!mainTitleElement) return;

        // 获取当前激活的导航项
        const activeNav = document.querySelector('.nav-link.active');
        if (!activeNav) return;

        // 根据激活的导航项ID确定翻译键
        const navId = activeNav.id.replace('_m', ''); // 移除移动端后缀
        const titleMap = {
            'basicInfo': 'nav.basicInfo',
            'serialConfig': 'nav.serialConfig',
            'sysConfig': 'nav.sysConfig',
            'protocolConfig': 'nav.protocolConfig',
            'netConfig': 'nav.netConfig',
            'logConfig': 'nav.logConfig'
        };

        const i18nKey = titleMap[navId];
        if (i18nKey) {
            mainTitleElement.textContent = this.t(i18nKey);
        }
    },

    applyBrandStyle() {
        // 根据品牌类型显示或隐藏品牌元素
        const brandElements = document.querySelectorAll('[data-brand="SHUOXIN"]');

        if (this.brandConfig.brand === 'SHUOXIN') {
            // 硕芯版本：显示LOGO和官网链接
            brandElements.forEach(el => {
                el.style.display = '';
            });
        } else {
            // 中性版本：隐藏LOGO和官网链接
            brandElements.forEach(el => {
                el.style.display = 'none';
            });
        }
    },

    async switchLanguage() {
        const newLang = this.currentLang === 'zh-CN' ? 'en-US' : 'zh-CN';
        await this.loadLanguage(newLang);

        // 更新按钮文字（在语言加载完成后）
        this.updateLangButton();
    },

    updateLangButton() {
        const langBtn = document.getElementById('langText');
        if (langBtn) {
            // 显示目标语言（当前是中文，显示EN表示点击后切换到英文）
            langBtn.textContent = this.currentLang === 'zh-CN' ? 'EN' : '中文';
        }
    },

    // 提供给外部调用的方法，用于动态添加的内容
    translateElement(element) {
        if (!element) return;

        const key = element.getAttribute('data-i18n');
        if (key) {
            const translation = this.t(key);
            const hasChildElements = element.children.length > 0;

            if (hasChildElements) {
                this.updateTextNodes(element, translation);
            } else {
                element.textContent = translation;
            }
        }

        // 递归翻译子元素
        element.querySelectorAll('[data-i18n]').forEach(child => {
            this.translateElement(child);
        });
    }
};

// 页面加载完成后初始化国际化
document.addEventListener('DOMContentLoaded', () => {
    i18n.init().then(() => {
        // 初始化完成后更新按钮
        i18n.updateLangButton();
    });

    // 添加语言切换按钮事件
    const langBtn = document.getElementById('langSwitch');
    if (langBtn) {
        langBtn.addEventListener('click', () => {
            i18n.switchLanguage();
        });
    }
});

// 导出到全局，供其他脚本使用
window.i18n = i18n;
